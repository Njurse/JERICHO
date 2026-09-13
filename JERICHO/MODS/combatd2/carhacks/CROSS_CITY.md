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

The car colours are *not* in the level file: `LoadCosmetics` (`cosmetic.c:93`)
reads them from a separate 3120-byte `LEVELS\<CITY>.LCF` (`CosmeticFiles[]`).
(`LUMP_PALLET` = 25 looks like the obvious candidate but is texture palettes —
`texture.c` consumes it via `ProcessPalletLump`.)

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

## Where this is implemented

`models.c` owns it: `InitCarImport()` (called from `SetupResidentModels` right
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
