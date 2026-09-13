# Cross-city vehicle imports — how the level file works

Design notes for importing another city's vehicles into a level. Written from
measurements of the data in `DRIVER2/LEVELS/*.LEV` and `MLEVELS/*.LEV`, before
any code changes.

## Where a level's car models actually live

Each city's car geometry is in **that city's own level file** — `LEVELS\<CITY>.LEV`
(single-player) or `MLEVELS\<CITY>.LEV` (multiplayer arenas). It is *not* in the
loose `LEVELS\<city>\CARMODEL_*` files: those are only an override the engine
consults when present (`gContentOverride`), and nothing in the tree currently
ships in the name the engine asks for.

So "a car from another city" means reading that city's level file. No external
tool or data export is needed.

## Reading a foreign level file

Plain stdio — no CD layer is involved on PC (`system.c`, and `loadsectorsPC`):

1. `fopen(gDataFolder + LevelFiles[city], "rb")`.
2. `fseek(f, 8, SEEK_CUR)` — skip the outer LUMP type+size header.
3. `fread` the 4 `XYPAIR` `citylumps[city]` entries (`citylumps[8][4]`,
   `system.h`): **DATA1 = index 0, TPAGE = 1, DATA2 = 2, SPOOL = 3**, each an
   (x = byte offset, y = byte size) pair into the file.
4. Read the **DATA1** region (`(x, y)`) into a buffer with a plain
   `fseek(x) + fread(y)`.
5. DATA1's body is itself a container lump (`LUMP_LEVELDESC = 35`), so skip 8
   more bytes and walk what follows. DATA2's body is `LUMP_LEVELDATA = 36`
   (`LUMP_MODELS = 1` and the general level data live there).

## Walking the segments (the detail that matters)

`ProcessLumps` (`Game/C/main.c:190`) walks a flat segment list: `type = *(int*)ptr`,
`size = *(int*)(ptr + 4)`, body at `ptr + 8`. Its advance is:

```c
lump_ptr = (char*)ptr + ((seg_size + 3) & ~0x3);   // main.c:378
```

**Segments are 4-byte aligned.** Walking with a raw `size + 8` drifts as soon as
a segment has a size that is not a multiple of 4 (a type-12 segment of 2163
bytes knocked a naive walk into garbage on its fourth step). Any code reading a
foreign `.LEV` must align the same way.

## What to lift out

| Lump | Id | What it is |
|---|---|---|
| `LUMP_CAR_MODELS` | 28 | Per-model offset table (3 ints per model: clean/damaged/low) then the geometry. This is the block `ProcessCarModelLump` consumes. Lives in **DATA1**. |

The car colours come in **two** pieces, and an import needs both:

| Piece | Where | What |
|---|---|---|
| `CAR_COSMETICS` | `LEVELS\<CITY>.LCF` (3120 bytes, via `CosmeticFiles[]`) | per-model colour metadata, read by `LoadCosmetics` (`cosmetic.c:93`) |
| `LUMP_PALLET` | id 25, in the level file's **DATA1** | the actual car palettes, merged into `civ_clut` by `ProcessPalletLump` (`cars.c:1427`) |

`LUMP_PALLET` is *not* texture palettes — despite the generic name it is processed
by `cars.c`, not `texture.c`, and `civ_clut[8][32][6]` is what car polygons read
their colours from (`cars.c` draw path: `pciv_clut[(clut_uv0 >> 0x10) + palette]`).

### Why a foreign palette needs mapping, not just merging

`GetCarPalIndex(tpage)` (`cars.c:1876`) resolves a texture page to one of eight
car-palette slots **through the current level's table**, `carTpages[GameLevel][8]`
— every city maps its *own* page numbers onto the same eight slots. So a page
belonging to another city is unknown to the host level and falls back to slot 0,
and the imported vehicle gets painted with the host's palette.

The fix has two halves, both keyed on the imported city:

1. its `LUMP_PALLET` is merged with **that city's** mapping
   (`ProcessPalletLumpForCity(..., city)`), so entries land where its own
   vehicles will look for them;
2. `GetCarPalIndex` falls back to the imported city's table for a page the host
   level does not know.

Measured car-palette pages per city: CHICAGO {1,50,62,63,65}, HAVANA
{10,20,35,37,51}, RIO {55,57,58,60,68}, VEGAS {17,32,41,54,62} — effectively
disjoint (only page 62 is shared, and the host's own lookup is tried first).

`ProcessCarModelLump` (`models.c:220`) indexes the car-models block as
`lump_ptr + 4 + model_number * 3 * sizeof(int)`, calls `GetCarModel(mem,
&mallocptr, 1)` and `buildNewCarFromModel(slot, ...)`, which **copy into the
level heap** (`mallocptr`). The foreign buffer is therefore only needed during
the build and can be freed straight after.

## Costs

Measured `LUMP_CAR_MODELS` sizes (bytes):

| City | LEVELS (SP) | MLEVELS (MP arena) |
|---|---|---|
| CHICAGO | 123324 | 109852 |
| HAVANA | 131412 | 119012 |
| RIO | 135688 | 135108 |
| VEGAS | 132252 | 132684 |

The MP figures are exactly the four values the engine logs at runtime
(`LUMP_CAR_MODELS: size: …`), which is what validates this walk. So: **~123-136 KB
per city, transient** — one foreign city at a time, freed once the models are
built.

## Two traps

- **The special slot ignores the per-city colours.** `car_cosmetics[SPECIAL_CAR_SLOT]`
is not taken per model — the cache `levelSpecCosmetics[model - 8]` is filled once
for models 8..12 (`cosmetic.c:140-160`). A foreign model placed in the special
slot would silently wear the **host** city's colours. `GetCarImportCosmetics()`
is therefore consulted *before* either path, so an imported slot takes its own
city's `CAR_COSMETICS` whichever slot it lands in.
- **Denting has no lump.** There is no denting entry anywhere in the lump enum
  (`main.c:80-130`); `LoadCustomCarDentingFromFile` reads loose `.DEN` files
  (`denting.c:454, 487`). Denting therefore stays local — an imported model
  dents as its host slot would.

## Turning it on

In `JERICHO/CONFIG/carhacks.ini` (off by default):

```
cross_city_vehicles = 1
import = 2:0:10
```

`import` takes comma-separated `slot:city:model` entries — here, Chicago's school
bus (model 10) into resident slot 2. City numbers are `0` CHICAGO, `1` HAVANA,
`2` VEGAS, `3` RIO. Slots 0..4 feed ambient traffic, 5..6 are spare capacity and
7 is the special slot; several entries may name different cities.

Verified in the log by the size of the block that was actually read — with
`import = 2:0:10` on a **Havana** level the engine reports

```
[carhacks] import: slot 2 <- model 10 from CHICAGO
cross-city: car data from CHICAGO (123324 bytes of models, 4096 of cosmetics)
```

and 123324 is Chicago's car-models block in the table above (Havana's own is
131412), so the foreign file really is what got loaded.

## Where this is implemented`models.c` owns it: `InitCarImport()` (called from `SetupResidentModels` right
after the query) reads the foreign level file and the foreign `.LCF` and holds
them for the level; `GetCarImportModels(slot)` / `GetCarImportCosmetics(slot)`
answer NULL for every slot the module did not import from, so a stock level takes
exactly its old path. Both consumers (`ProcessCarModelLump`, `ProcessCosmeticsLump`)
swap only their base pointer, because the foreign block has the same layout as
the level's own.

## Related

The existing folder override (`GetCarDataFolder()`, `JER_EVENT_CAR_DATA_SOURCE`)
redirects the **loose-file** loaders only (`.MDL` / `.COS` / `.DEN`). On its own
it cannot produce foreign vehicles, because there is no loose vehicle data to
find. It stays as the hand-made-car path.
