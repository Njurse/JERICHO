# How car palettes work, end to end

The canonical reference for **where a car's colour comes from**. Read this before
touching anything that resolves a colour, swaps a palette, or reasons about which
`civ_clut` row a set belongs to.

Line references are into `src_rebuild/Game/` and are the real thing as of the
commit that added this file — if a citation has drifted, fix it rather than
trusting it. The **byte layout** of a city's palette file lives in `FORMATS.md`
§4 (it owns the file formats); this file owns the *resolution* — which row, which
CLUT, and where in VRAM.

Everything below is measured off this install's data or read straight from the
engine's own runtime log, not guessed. Where a number is stated, a recipe to
re-measure it is in `FORMATS.md` §7 and in the tools section at the end.

---

## 1. The two tables a car's colour comes out of

A car polygon carries a **`texture_set`** and a **`texture_id`** (both one byte).
They are not addresses; they index two engine arrays that a level fills on load:

```c
u_short texture_pages[128];        // texture.c:95   set index -> VRAM tpage handle
u_short texture_cluts[128][32];    // texture.c:96   set index -> CLUT handle per texture id
u_char  tpageloaded[128];          // texture.c:97   0 = "not loaded" / slot+1 while loaded
```

and a car's *paint* comes out of a third table, `civ_clut`:

```c
u_short civ_clut[CIV_CLUT_ROWS][32][6];   // cars.c:104   [car palette row][texture id][colour slot]
```

with

```c
#define CIV_CLUT_ROWS        16   // cars.h:44   rows 0..7 host level, 8..15 an import
#define CIV_CLUT_IMPORT_ROW   8   // cars.h:45
```

**The non-obvious bit.** A GT poly's `clut_uv0` high word is **not** a CLUT id —
it is an **index into `civ_clut`**. `buildNewCarFromModel` computes it and the
draw reads it back:

```
build:  carid = GetCarPalIndex(pgt3->texture_set);                  cars.c:1319
        clut  = (carid - 1) * 6 * 32 + pgt3->texture_id * 6;        cars.c:1320   (= (carid-1)*192 + texid*6)
        cp->clut_uv0 = M_INT_2(clut, uv0);                          cars.c:1326
draw:   pg->pciv_clut[(src->clut_uv0 >> 0x10) + palette]            cars.c:289
```

`pciv_clut` is not `civ_clut`; it is `&civ_clut[1]` (`cars.c:1046`) — i.e. offset
by one **row** (a row is `32*6 = 192` shorts). So

```
pciv_clut[k] = civ_clut_flat[192 + k]
```

and `(carid-1)*192 + texid*6` at `k` lands on `civ_clut_flat[carid*192 + texid*6]`,
i.e. exactly **`civ_clut[carid][texid][palette]`**. That one row offset is why the
`carid` name reads as a 1-based row and why the formula subtracts 1.

Three consequences worth internalising:

- **The row is chosen by `carid` = which of the *city's* eight car pages this set
  is** — see §3. It is not baked into the geometry; it is *computed*, so it can be
  re-pointed by changing which row a page maps to.
- **`texture_cluts` is the base slot.** Slot 0 of a row is filled per frame from
  the page's own CLUTs (`cars.c:1322`, `:1348`), slots 1..5 come from
  `LUMP_PALLET` (§4). A GT poly with `palette = 0` therefore draws the *page's*
  CLUT; `palette = N` draws the N-th paint variant.
- **FT polys differ.** FT3/FT4 emit `clut_uv0` **verbatim** from
  `texture_cluts[set][texid]` (`cars.c:1280`, `:1296`) — for an FT poly the high
  word really *is* a CLUT id, and no `civ_clut` row is read.

The `palette` in `… + palette` is the **car's colour choice**, not the row:
`DrawCarObject` passes `cp->ap.palette` (`cars.c:1887`, `:1913`), and a spawned
civilian picks it as `Random2(0) % 6` (`civ_ai.c:2278`). It selects one of the six
colour slots within the resolved row.

---

## 2. Where the CLUT *ids* in a row come from

`ProcessPalletLump` (`cars.c:1570`) walks the host level's `LUMP_PALLET` and
`ProcessImportedPalette` (`cars.c:1584`) walks an imported city's, both through
`ProcessPalletLumpForCity` (`cars.c:1496`). Per record the engine does one of two
things (`cars.c:1517-1562`):

```c
if (clut_number == -1)                      // the CLUT image follows inline
{
    LoadImage(&clutpos, (u_long*)buffPtr);          // cars.c:1539  upload the 32 bytes into VRAM
    clutValue = GetClut(clutpos.x, clutpos.y);      // cars.c:1542  the id of where it landed
    IncrementClutNum(&clutpos);                     // cars.c:1543  advance the strip cursor
}
else
    clutValue = clutTable[clut_number];             // cars.c:1551  reuse an earlier upload

civ_clut[palidx][texnum][palette + 1] = clutValue;  // cars.c:1560  NOTE: palette + 1
```

So a `LUMP_PALLET` entry carries either a CLUT image (which the engine uploads
into the shared strip and remembers the id of) or a back-reference to one it has
already uploaded. `palidx` is `CarPalIndexInCity(tpageindex, city)` (`cars.c:1555`)
— the **same** row rule the draw uses, which is what keeps writer and reader in
step.

`load_civ_palettes` (`texture.c:742`) is a **no-op** (`return;`) — the palette
area is filled by `LUMP_PALLET` alone.

---

## 3. `carTpages` / `specTpages`: a page number means a different thing per city

Which of the eight rows a page maps to is a **per-city table**. Every city
renumbers its own art onto the same eight slots, so a `texture_set` number is only
meaningful together with the city:

```c
char carTpages[4][8];    // texture.c:71   [city][palette slot] -> texture_set
char specTpages[4][12];  // texture.c:36   [city] -> 6 special bodies x 2 pages
```

`CarPalIndexInCity` (`cars.c:1954`) is nothing more than "which of `carTpages[city][0..7]`
equals this set", plus the import bank:

```c
rowbase = (city == GetCarImportCity() && city != GameLevel) ? CIV_CLUT_IMPORT_ROW : 0;  // cars.c:1962
for (i = 0; i < 8; i++)
    if (tpage == carTpages[city][i]) return i + rowbase;                                // cars.c:1964-1968
```

and `GetCarPalIndex` (`cars.c:1973`) is the host's table, falling back to the
imported city's table, then `0`:

```c
idx = CarPalIndexInCity(tpage, GameLevel);      // cars.c:1975
if (idx >= 0) return idx;
idx = CarPalIndexInCity(tpage, GetCarImportCity());   // cars.c:1991  an imported page
return (idx >= 0) ? idx : 0;                    // cars.c:1997  not a car page anywhere
```

Measured car-page sets (from each city's `LUMP_PALLET` `tpageindex` field):

| City | car texture sets |
|---|---|
| CHICAGO | 1, 50, 62, 63, 65 |
| HAVANA | 10, 20, 35, 37, 51 |
| RIO | 55, 57, 58, 60, 68 |
| VEGAS | 17, 32, 41, 54, 62 |

Only **62** is shared (CHICAGO/VEGAS), so `GetCarPalIndex` must check the host
table first or a host car would be repainted with a foreign row.

**A trap that matters for imports:** `LoadPermanentTPages` *overwrites* two of the
host's entries at runtime —

```c
carTpages[GameLevel][6] = page1;   // texture.c:2025  the host's special body's own two pages
carTpages[GameLevel][7] = page2;   // texture.c:2026
```

so slots 6/7 of the host's table are the special car's pages, not `carTpages[]`
entries parsed from data. And a **special** body's pages come from `specTpages`,
which is a *different* table — for a long time `CarPalIndexInCity` only scanned
`carTpages`, so a `specTpages` page was found in neither city and `GetCarPalIndex`
returned **0** — the host's row, which `CarImportPin` then overwrote. **Fixed:**
`CarPalIndexInCity` now scans `specTpages` too and maps the pair onto the bank's last
two rows (`rowbase + 6 + (i & 1)`), which is exactly where the host's own pair sits
(`cars.c:1954-1998`). A set that is in *neither* table still answers 0 — as it does for
a host car — so the pin refuses to write that row and logs it (§5).

---

## 4. The `clutpos` strip: one column, carved in a fixed order

`clutpos` (`texture.c:102`) is a cursor into the **CLUT column at the right of
VRAM**: `x = 960..1023` (four CLUTs per scanline, 16 px each) and `y = 256..511`.
`IncrementClutNum` (`texture.c:125`) advances by one CLUT, wrapping to the next
scanline:

```c
clut->x += 16;
if (clut->x >= 1024) { clut->x = 960; clut->y += 1; }
```

`LoadPermanentTPages` (`texture.c:1935`) fills this column **in this order**, and
each step starts where the previous one stopped:

| # | Step | Where |
|---|---|---|
| 1 | `clutpos` = (960,256) | `texture.c:1962-1965` |
| 2 | two increments, then `fontclutpos = clutpos` | `texture.c:1977-1980` |
| 3 | host car palettes (`ProcessPalletLump`) | `texture.c:1981` |
| 4 | **imported car palettes** (`ProcessImportedPalette`) | `texture.c:1982` |
| 5 | `load_civ_palettes` — no-op | `texture.c:1984` |
| 6 | the level's permanent page CLUTs | `texture.c:1996-2005` |
| 7 | the special body's page CLUTs | `texture.c:2044-2072` |
| 8 | realign x to 960, y++ | `texture.c:2081-2085` |
| 9 | **the streamed-slot band**: `slot_clutpos[i] = clutpos`, then `clutpos.y += 8` per slot | `texture.c:2088-2100` |

So the **streamed slots' CLUT rows begin strictly below everything the level and
the import uploaded**, and each of the 19 texture slots gets an 8-row band. The
`mapclutpos` / `fontclutpos` RECTs (`texture.c:103-104`) share the same column but
are separate cursors.

`slot_clutpos[]` is read back by the streamer when it uploads a region page's
CLUTs: `SendTPage` uses `slot_clutpos[slot]` (`spool.c:491-492`) and
`SpecClutsSpooled` uses `slot_clutpos[specialSlot]` (`spool.c:1712-1713`).

**Why this matters:** the strip is a single 256-row budget shared by the host
palettes, the import's palettes, the level's page CLUTs and the world's streamed
CLUTs. Measured end-of-load `clutpos`: **428 stock, 485 with a Havana->Rio import**
(the import's palettes and page CLUTs consumed 57 of the 256 rows, leaving 27). `CAR_CLUT_IMPORT_LIMIT = 476`
(`cars.h:51`) exists only to stop an import walking the cursor into the band the
per-frame pin reserves — past it, `ProcessPalletLumpForCity` reuses the city's
first palette for the remaining entries and logs how many (`cars.c:1532-1536`,
`:1564-1566`). That is a band-aid: the import and the world are competing for one
budget.

> The column's geometry, the whole VRAM budget around it, and the collision it has
> with the level font image are in **`VRAM.md`** — including the measured
> `clutpos.y` for a stock level (428) and with an import (485), and the section of
> `tools/vrammap.py` that prints which claims sit in the import's reserved rows.

---

## 5. The per-frame re-point (the load-time cache is stale)

`buildNewCarFromModel` runs at **load** time and caches slot 0 of each row from
`texture_cluts[set][texid]` (`cars.c:1322`, `:1348`). At that moment an *imported*
page has not been uploaded yet, so `texture_cluts[set]` still holds the dummy
`(960,16)` and the cached id is the dummy's own id (`043c`). The page's real CLUTs
only exist after the import uploads it at draw time, so `CarImportPin`
(`texture.c:1332`) re-points the row:

```c
int row = GetCarPalIndex(sPinSet[i]);                 // texture.c:~1566

if (row < CIV_CLUT_IMPORT_ROW)                        // a HOST row: refuse, and say so
    printInfo("cross-city: pin - set %d resolves to civ_clut row %d (a HOST row): not re-pointing, palette leak avoided\n", ...);
else
    for (j = 0; j < 32; j++)
        civ_clut[row][j][0] = texture_cluts[sPinIndex[i]][j];
```

That single line is the load-time/draw-time seam. It is safe while `row` is one of
the import's own rows (8..15) — and it is a **leak** when `row` comes back 0 for a
special-body import (§3), because row 0 is the host's.

The import's own CLUT rows come from a second, separate cursor (`sPinClutCursor`,
`texture.c:1441-1461`) that starts at `max(clutpos.y + 4, 480)` and is bounded
below 512, so the imported *pages*' CLUTs never land in the level's band.

---

## 6. The check that is supposed to catch all this — and doesn't

`LoadPermanentTPages` hashes `civ_clut` into the log as the cross-city invariant
(`texture.c:2110-2117`):

```c
for (i = 0; i < CIV_CLUT_ROWS * 32 * 6; i++)
    clutSum = clutSum * 31 + ((u_short*)civ_clut)[i];
printInfo("cross-city: level page state - slotsused=%d nperms=%d nspecpages=%d tpage=(%d,%d) clutpos=(%d,%d) civclut=%08x\n", ...);
```

It now prints **two** sums: `civclut` for rows 0..7 (the HOST's rows — the number that
means the same thing across builds, so a stock run must print the same value before and
after a change) and `civclut16` for all `CIV_CLUT_ROWS` rows (so the import bank is
visible too, comparable only within one build). What neither does:

A useful run proves three things instead:

1. no imported set resolves into a *host* `civ_clut` row;
2. no imported page occupies a VRAM rectangle the world streams into;
3. a world set's `texture_pages[]` / `texture_cluts[]` agree with the pixels
   actually at its rectangle (see `tools/vramdump.py`).

---

## 7. The tools

| what | how |
|---|---|
| what a city's car palettes *should* look like | `tools/levpalette.py <CITY>.LEV` — reads `LUMP_PALLET` and writes a swatch sheet per city + the raw rows (`--out DIR`) |
| what a car's texture looks like under **each** of its palettes, from the **last run** | `tools/cardump.py` — takes the run's `vram_dump.tga` (`JERICHO_DUMPVRAM=1`) plus the run's log, and blits each imported car's texture page(s) through each palette to one PNG per palette |
| whether a run's palettes/pages actually behaved | `tools/crosscheck.py <run text> [--tga vram_dump.tga] [--lev SRC.LEV]` — asserts the three invariants in §6 (exit 0 held / 1 violated / 2 no import) |
| what is actually in VRAM right now | `-vramview [frames]` (live window) / `tools/vramdump.py vram_dump.tga --png out.png` |
| which sets/rows a level uses | the engine's own lines: `cross-city: level page state …`, `cross-city: %s set %d -> index %d …`, `cross-city: pinned set %d index %d: slot=%d, rect=(%d,%d) …` |

The exporter tools are how a fix is shown *visually*: `levpalette.py` gives the
expected colours offline, `cardump.py` gives what the last run actually drew under
each palette, and the two are meant to be compared side by side. `crosscheck.py` is
how it is shown *measurably* — it is the check §6 says the engine's own summary is
missing.

> **Which log file?** The session log is `<appName>.log`, and this build's app name is
> JERICHO (`PsyX_Initialise("JERICHO", …)`, `PsyX_main.cpp:380`), so the live file is
> `JERICHO.log`. A stale `REDRIVER2.log` from an older build name may still be in the
> folder — do not grep it. Capturing the run's stdout works just as well (and is what
> `devcheck.sh` does) because `printInfo` writes both.

---

## 8. Gotchas, collected

- A GT `clut_uv0` high word is a `civ_clut` **index** (`(carid-1)*192 + texid*6`),
  not a CLUT id; an FT one **is** a CLUT id. `pciv_clut = &civ_clut[1]` (one
  *row*) makes the GT formula land on `civ_clut[carid][texid][palette]`.
- `LUMP_PALLET` writes `palette + 1`; slot 0 of a row is the page's own CLUT and is
  refilled per frame (`cars.c:1322`, `:1348`, `texture.c:1511`).
- `carTpages[GameLevel][6..7]` are **overwritten** with the special body's pages at
  runtime (`texture.c:2025-2026`).
- A `specTpages` page is not in `carTpages` — that used to make `GetCarPalIndex`
  answer 0 (the host's row); it is now scanned and mapped onto the bank's last two rows.
- A set in **neither** table answers row 0, exactly as a host car's does — the pin
  refuses to *write* that row and logs it.
- Two sets imported in one load saw `110` free for both and both took it, collapsing a
  car's two pages onto one index; `FindFreeSetIndex` now reserves what it hands out.
- `ProcessPalletLumpForCity` for an import uploads into the **shared** `clutpos`
  strip (the world's streamed CLUTs are carved from the same column) and can be
  truncated by `CAR_CLUT_IMPORT_LIMIT`.
- `JerichoMakeClutRow` (`texture.c:153`, team/ped dye) allocates rows from the same
  runtime `clutpos` cursor (`texture.c:342-345`), guarded only by `clutpos.y > 511`.
- The cross-city invariant hashes `civ_clut` (all rows now) and says nothing about VRAM.
- The session log is `JERICHO.log` in this build, **not** `REDRIVER2.log`; a stale
  `REDRIVER2.log` can sit in the same folder.
