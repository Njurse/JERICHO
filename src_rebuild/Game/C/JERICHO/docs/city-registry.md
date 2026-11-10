# The city registry — arbitrary car-data banks

**What this is.** A *city index* used to be a compile-time row of the engine's
three tables (`LevelNames`, `LevelFiles`, `CosmeticFiles`, `system.c:151` /
`cosmetic.c:14` at the time of writing). The engine could therefore only ever
open a car-data file it had been **compiled** to name — `LEVELS\CHICAGO.LEV`,
`JERICHO\MODS\carhacks\D1CARS\MIAMI.LEV` and nothing else. carhacks' cross-city import is *policy*
("resident slot 5 = city 2, model 7") and is already index-based, so a bank of
cars that is not one of those names could not be offered at all: nothing would
ever open the file, whatever the config said.

The city registry closes that. Up to **16 arbitrary banks** are registered at
runtime, each on its own city index, from one config file. Everything carhacks
already does with a city then works on them unchanged.

## The config

`JERICHO/CONFIG/cities.ini`, one line per bank:

```ini
[cities]
city_0 = BETTERCHI, LEVELS\BCCHI.LEV, LEVELS\BCCHI.LCF
city_1 = MYBANK,   MODS\banks\MB.LEV
```

```
city_N = NAME, <car-data file>[, <colours file>]
```

| field | meaning |
|---|---|
| `city_N` | the bank's **city index is fixed**: `11 + N`. The stock eleven are `0..10` — Driver 2's four (CHICAGO, HAVANA, VEGAS, RIO) then Driver 1's seven car-data cities (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE, IVIEW, TRAIN). |
| `NAME` | what logs and menus show. A bank is *not* playable, so it never becomes a `GameLevel`. |
| car-data file | the `.LEV` carrying `LUMP_CAR_MODELS`, `LUMP_PALLET` and `LUMP_TEXTUREINFO` — the same shape a city's level file has. |
| colours file | optional `.LCF` (3120 bytes). Absent = the cars keep the cosmetics derived from their model. |

Paths are **relative to the game's data folder** (`DRIVER2\`), may use either
slash style, and may step out of it — the BetterCars banks beside `DRIVER2\`
are `..\BetterCars\BCCHI.LEV`. The engine builds the path with
`JerCityPath()` (data root + entry + `FS_FixPathSlashes`), which is the single
place a city's file is resolved; `LoadCarImport`, `ReadCarImportFileForCity` and
the registry's own presence probe all go through it.

## It follows the disk, with no restart

* A bank is registered **only when its car-data file opens**. A configured bank
  whose files are missing is not a city: it is logged once
  (`city registry: X is not installed (no ...) - not offered`) and its row stays
  empty.
* The list is re-scanned at **boot**, before **every frontend car list**
  (`FEmain.c`, just before `JER_EVENT_CAR_AVAILABILITY` fires) and at **every
  level load** (`InitCarImport`). Install the files and the bank appears; delete
  them and it is gone — no restart, and nothing dangles.
* **`city_N` keeps its index** whichever of the others are installed, so an
  import config that names city 10 names the same bank tomorrow. A missing bank
  therefore leaves a **hole** in the row range: loop a user-visible list to
  `JerCityCount()` and skip every row where `JerCityRegistered()` is 0. A row
  that is not registered has NULL table entries, and `JerCityPath` refuses it, so
  no `NULL` can reach a `%s`.

## Module-owned banks

A module may register a bank itself with `JerCityRegister(...)`. carhacks does
this for Beta Miami (`JERICHO/MODS/carhacks/D1CARS/BETAMIAMI.LEV`), so a bake is
offered automatically without a `cities.ini` edit. Such a row is **module-owned**:
it has no `city_N` key, and the config refresh re-probes its file instead of
clearing it. A `city_N` key for the same slot takes ownership instead, and
`JerCityRegister(name, NULL, NULL)` removes a module row. It takes the first free
bank index, so it appears after the stock eleven cities.

## What a bank must actually contain

The car-model block is the engine's own `LUMP_CAR_MODELS` layout: a count, then
13 entries of three int32 offsets (clean / damaged / low) relative to the block.
`GetCarImportModels` requires **all three** variants of a model, so a model with
only some of them reads as absent — the slot keeps the level's own car and the
log says so (`asked for model N from X, which has no such model`).

A bank may legitimately carry only a **few** models — a retexture pack that
replaces two vehicles puts two models in the block, not thirteen, with `-1` for
the rest. Nothing needs to be fabricated: the registry's model query
(`JerCityCarModels`) reports what is really there, and carhacks offers exactly
that (`car list: BCCHI ... offering the 2 car(s) its imported data carries`).

Banks also have no `M<CITY>.LEV` arena variant — only Driver 2's own four cities
do — and no `LEVELS\<CITY>` override folder, so per-car `CARMODEL_*` overrides do
not apply to them.

## Car-select portrait

Stock Driver 2 portraits are not in the `.LEV`. They are loose pages in
`DATA\CARS\CCARS.RAW` / `HCARS.RAW` / `VCARS.RAW` / `RCARS.RAW` (one `0x8000`-byte
4bpp page per frontend slot, drawn by `SetupExtraPoly` in `FEmain.c`). A bank and
a Driver 1 city have no such file, so the car-select menu used to leave their
icon blank.

The menu asks for a city and a **model number** (the in-game model, not the
dense roster cursor). The frontend looks beside the `.LEV` the registry opens,
in this order:

1. `<directory of the .LEV>/<LEV stem>.RAW`, page at offset `model * 0x8000`,
   when that file exists and the page is not all zero. A car-bank bake writes
   this next to `<stem>.LEV` / `<stem>.LCF`. It is the same page size as one
   slot of `CCARS.RAW`. It is not a lump in the LEV. Stock `CCARS.RAW` is
   indexed by the roster cursor; this file is indexed by the model, because
   that is the number the menu already passes.
2. `<LEV stem>.portraits/<model>.raw` — the loose page. A car-bank bake still
   writes it, and a hand-dropped folder still works. It is the fallback when
   the sibling RAW is missing or that page is all zero.
3. Driver 2 cities 0–3: that model's stock slot in `CCARS` / `HCARS` / `VCARS` /
   `RCARS`.
4. Everywhere else, one shared page (below).

`BAKE_D1_CARS` encodes Miami's authored
`assets/portraits/MIAMI/portraits/MCARS.RAW_<n>.RAW.png` through the bake
portrait packer and writes `D1CARS/MIAMI.RAW`. Page `n` is source car `n`.
The city bake lays those cars on models 1, 2, 3, 4, 8, 9, 10, and the page
sits at `model * 0x8000`. Slots with no picture, including 0 and 5–7, are
zero. A mode-P PNG already on the car-select CLUT keeps its indices. The
package ships those PNGs and `portrait.py`. It does not ship `D1CARS`. A
bank whose cars are already current and whose `MIAMI.RAW` is missing is
asked to encode the pictures only; `BAKE_VERSION` stays put.

```
DRIVER2\LEVELS\BCCHI.LEV → BCCHI.RAW page 10, else BCCHI.portraits\10.raw
JERICHO\MODS\carhacks\D1CARS\MIAMI.LEV → MIAMI.portraits\1.raw
```

Miami has no `MIAMI.RAW`. Those pages stay in `MIAMI.portraits\<model>.raw`
(model 1 is the cop, which is roster slot 0). Do not rename them. A missing
RAW is the normal case and the loose files still load.

Each page is exactly 32768 bytes (256×256 at 4bpp). The frontend shows the top
219 rows as a 255×219 sprite and takes the colours from `DATA\CARS\CARBACK.RAW`,
unless `<model>.clut` (32 bytes, 16 little-endian RGB555) sits in the portraits
folder — that replaces the portrait CLUT at VRAM `(960,256)` for the draw,
whichever pixel source won. The RAW does not carry CLUTs. Omit the `.clut`
when the indices were packed for that stock CLUT.

An all-zero page in the RAW counts as missing, so the loose file and then the
stock slot or the shared page still run. A missing file on Driver 2 cities 0–3
still draws that model's stock slot. Everywhere else (a Driver 1 city, a bank,
or a model the ten-slot lookup does not name) draws one shared page, indexed
for the same CLUT:

```
JERICHO\MODS\carhacks\assets\portraits\CARHACKS\fallback.raw
```

That is carhacks' own asset, so it is always read from the carhacks folder,
even when the Driver 1 LEVs were found somewhere else (see "Where the Driver 1
cars are read from" below). The stock
mission car screen is unchanged; the module menu is the one that asks for a
city and a model. Miami's pages and the screenshot encoder are in carhacks
`docs/PORTRAITS.md`.

**A sparse bank can be completed.** Copying the missing models out of the host
city's own level file (byte for byte, plus the page sets and palette rows they
need) is `JERICHO/MODS/carhacks/tools/bankfill.py`; run its `--report` first to see
what a bank lacks and `--verify` after writing. The rules it follows — including
why a copied model's texture ids need no rewrite — are in
[`BANKFILL.md`](../../../../../JERICHO/MODS/carhacks/docs/BANKFILL.md).

## Where the Driver 1 cars are read from

The seven stock Driver 1 rows (and Beta Miami) read `D1CARS\<CITY>.LEV` under
`gD1DataFolder`. A bake writes `JERICHO\MODS\carhacks\D1CARS`, but the engine
reads from wherever a valid bake already is. At startup `JerD1DataFolderInit`
(system.c) asks `jer_d1cars_scan` (src/jer_d1cars.c, declared in
jer_rebake.h), which looks in this order:

1. `JERICHO\MODS\carhacks\D1CARS` (where a bake writes)
2. `d1cars_path` in `JERICHO\CONFIG\jericho.ini`: optional and unset by
   default. It names a shared folder that holds `D1CARS`, or the `D1CARS`
   folder itself, either absolute or relative to the game folder. The engine
   reads it and never writes it.
3. `DRIVER\D1CARS` (bakes from before the move to carhacks)
4. `<folder beside this install>\JERICHO\MODS\carhacks\D1CARS` (another
   JERICHO install, such as `JERICHO Release`)

A LEV counts only when the car importer could read it: the citylump table,
DATA1 inside the file, and a `LUMP_CAR_MODELS` segment with at least one
model. The newest `BAKE_VERSION` wins (a valid, unstamped bake counts as 0),
then the most valid LEVs, then the earliest place in the list. The log names
every candidate and the one chosen (`[d1cars] ...`).

A bank path spelled for the canonical folder
(`..\JERICHO\MODS\carhacks\D1CARS\...`, as carhacks and BAKE_D1_CARS write
Beta Miami) is mapped onto the chosen folder, so carhacks' own Beta Miami
registration finds the bake there. The re-bake prompt and
the mod store judge the chosen bake: "not baked" means no valid LEV anywhere,
and the version check uses the chosen folder's stamp. Nothing is copied, and
the only folder a re-bake writes is carhacks'.

## The API

`jer_city.h` (SDK + engine copies, kept identical by `tools/doccheck.py`):

| call | answers |
|---|---|
| `JerCityCount()` | how many city rows are in play (stock eleven + assigned bank rows). The bound to *loop* to. |
| `JerCityFirstCustom()` | 11 — the first row a bank can occupy. |
| `JerCityRegistered(city)` | 1 when the row has a name and a car-data file. |
| `JerCityName(city)` / `JerCityFile(city, kind)` | the name and the configured relative path ("" when none). |
| `JerCityPath(city, out, size, kind)` | the openable path, or 0 for an unregistered row. |
| `JerCityFileExists(city, kind)` | 1 when that file is on disk now. |
| `JerCityRegister(name, lev, lcf)` | register a bank from a module (returns its index, or -1). |
| `JerCityRegistryRefresh()` | re-read the config and re-probe the disk. |
| `JerCityCarModels(city)` | the model bitmask the bank carries, loading its car data first. |

`JerCityCount()` is a *sweep* bound, not a city count you can index blindly —
holes are normal (see above).

## Adding a bank, step by step

1. Put the bank where the config will point: either under `DRIVER2\` (a
   subfolder is fine) or beside it, addressed with `..\`.
2. Add the line to `JERICHO/CONFIG/cities.ini`.
3. Start the game (or rebuild nothing — this is pure config) and read
   `JERICHO.log`:
   ```
   city registry: city 9 = BCCHI, car data ..\BetterCars\BCCHI.LEV, colours ..\BetterCars\BCCHI.LCF
   city registry: 4 bank(s) registered, 13 city index(es) in all (city 9 is the first bank)
   ```
4. Ask for a car from it, e.g. `JERICHO/CONFIG/carhacks.ini`:
   `import = 5:9:10` (resident slot 5 = city 9, model 10). The engine's own
   verdict lines tell you what happened:
   ```
   cross-city: car data from BCCHI (41796 bytes of models, 10760 of car palettes, 3120 of cosmetics)
   cross-city: slot 5 geometry from BCCHI model 10
   cross-city: page check - slot 5 clean: 200 of 254 polys resolve to a pinned imported page
   ```
   A short page check means that car did not fully load; the palette lines say
   which panels fall back to the bank's base row.

## Traps worth knowing

* **`CITY_COUNT` is a capacity, not a count** (`system.h`): 27 rows, of which the
  last 16 are bank slots. Ask `JerCityCount()` for the count. Every per-city
  engine table (`gCarImports`, `carTpages`, `specTpages`, `CarAvailability`, …)
  is sized for the capacity.
* **The frontend city menu is still the stock four.** A bank is a car-data
  source, like Driver 1's seven: it is offered in the car list and the mp picker,
  never as a place to drive.
* **VRAM**: importing a bank costs texture pages exactly like importing a Driver
  1 city. Check `JERICHO-VRAM` / `-vramview` before adding several.
* **A missing bank is not an error.** Delete the file and it disappears from
  every list on the next car list or level load; the indices of the others do
  not move.
