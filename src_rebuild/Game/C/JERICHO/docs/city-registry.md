# The city registry — arbitrary car-data banks

**What this is.** A *city index* used to be a compile-time row of the engine's
three tables (`LevelNames`, `LevelFiles`, `CosmeticFiles`, `system.c:151` /
`cosmetic.c:14` at the time of writing). The engine could therefore only ever
open a car-data file it had been **compiled** to name — `LEVELS\CHICAGO.LEV`,
`D1CARS\MIAMI.LEV` and nothing else. carhacks' cross-city import is *policy*
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
| `city_N` | the bank's **city index is fixed**: `9 + N`. The stock nine are `0..8` — Driver 2's four (CHICAGO, HAVANA, VEGAS, RIO) then Driver 1's five car-data cities (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE). |
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

## The API

`jer_city.h` (SDK + engine copies, kept identical by `tools/doccheck.py`):

| call | answers |
|---|---|
| `JerCityCount()` | how many city rows are in play (stock nine + assigned bank rows). The bound to *loop* to. |
| `JerCityFirstCustom()` | 9 — the first row a bank can occupy. |
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

* **`CITY_COUNT` is a capacity, not a count** (`system.h`): 25 rows, of which the
  last 16 are bank slots. Ask `JerCityCount()` for the count. Every per-city
  engine table (`gCarImports`, `carTpages`, `specTpages`, `CarAvailability`, …)
  is sized for the capacity.
* **The frontend city menu is still the stock four.** A bank is a car-data
  source, like Driver 1's five: it is offered in the car list and the mp picker,
  never as a place to drive.
* **VRAM**: importing a bank costs texture pages exactly like importing a Driver
  1 city. Check `JERICHO-VRAM` / `-vramview` before adding several.
* **A missing bank is not an error.** Delete the file and it disappears from
  every list on the next car list or level load; the indices of the others do
  not move.
