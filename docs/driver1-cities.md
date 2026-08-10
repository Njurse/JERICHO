# Driver 1 cities in REDRIVER2 — architecture & format notes

Everything discovered while adding take-a-ride support for the four Driver 1
(PS1) cities — **Miami, San Francisco, Los Angeles, New York** — into this
fork of REDRIVER2. Written for whoever continues this work.

## 1. Why it works at all: D1 and D2 share a container

Driver 1 (1999) and Driver 2 (2000) were built on the same Reflexive-ish
PS1 engine. Their `.LEV` level files share the **top-level container**
exactly:

```
offset 0   LUMP header  { int type = 37 (LUMPDESC); int size = 2040 }
offset 8   OUT_CITYLUMP_INFO  4 × XYPAIR { offset, size }:
             [0] loadtime   (lumps processed at load time)
             [1] tpages     (permanent texture pages)
             [2] inmem      (lumps kept in RAM: models, cells, spoolinfo…)
             [3] spooled    (region streaming data)
```

REDRIVER2's `SetCityType()` (Game/C/system.c) already reads exactly this —
skip the first 8 bytes, read 32 bytes of citylumps. Verified by hex dump:
`MIAMI.LEV` and `CHICAGO.LEV` both start with `type=37, size=2040` and the
four offsets/sizes are sane for both.

The engine's tables are already sized for 8 cities:
`citylumps[8][4]`, `levelstartpos[8][4]` (entries 4–7 are the old D1 spawns,
commented `// what?`).

## 2. Where the D1 assets live

The full Driver 1 PS1 disc data was extracted next to the D2 data:

```
src_rebuild/bin/Release_dev/DRIVER/     <- D1 (new)
src_rebuild/bin/Release_dev/DRIVER2/    <- D2 (the game runs from here)
```

D1 city levels: `DRIVER/LEVELS/{MIAMI,FRISCO,LA,NEWYORK}.LEV` plus
`FLAT/CREDITS/IVIEW/TRAIN.LEV` (menu/easter-egg levels), `GFX`, `DATA`
(0.PBN–3.PBN car pics, SKY.BIN, FRUSTRUM.DAT), `SOUND`, `FRNT`, `XA`, `NFMV`.

The D1 data folder is configurable: `config.ini` → `[fs] d1DataFolder`
(default `DRIVER\`). The game's own config parser only knows `dataFolder`,
so the driver1 mod reads the key itself.

## 3. Inside a D1 level — lump inventory

Each loadtime/inmem section **begins with a 32-bit lump count** and has no
`0xff` terminator (D2 has the terminator and no count). Parsed inventory of
`MIAMI.LEV` (13.6 MB spooled, 10×14 regions):

```
loadtime (lump 35 wrapper):  11 lumps
   5  TEXTURENAMES   7 ROADMAP   12 MODELNAMES
  25  PALLET         2 MAP       22 MOTIONCAPTURE ×4
  34  TEXTUREINFO   28 CAR_MODELS
inmem (lump 36 wrapper):      11 lumps
   1  MODELS        26 SPOOLINFO   8 ROADS     9 JUNCTIONS
  16  ROADBOUNDS    17 JUNCBOUNDS  20 SUBDIVISION
  10  ROADSURF      24 OVERLAYMAP  33 CHAIR     21 LOWDETAILTABLE
```

vs `CHICAGO.LEV` inmem: `1, 26, 40(STRAIGHTS2), 41(CURVES2), 43(JUNCTIONS2),
20, 24, 33, 21` — the roads/junctions/bounds lumps are **the D1-only ones**,
and the engine still has handlers for them (map.c, mostly stubs — see §6).

Model counts: MIAMI 653, CHICAGO 1205 (first int of the MODELS lump).

## 4. The format differences that matter (all verified against the files)

| Area | Driver 2 | Driver 1 |
|---|---|---|
| section terminator | `0xff` lump | leading int lump count |
| map | cells 512×768 @2048, region 32 cells | MIAMI 300×420 @3000, region 30 cells (FRISCO/LA 540×540, NY 480×480) |
| spoolinfo lump | same layout | **identical** (parses to the byte: slots/objs/pvs/offsets/spoolbytes) |
| cell entry | 2-byte `{ushort num}` with in-band 0x4000/0x8000 markers | 4-byte `{ushort num; ushort next_ptr}` linked list, 0xffff ends |
| cell object | 8-byte `PACKED_CELL_OBJECT` (relative u16 pos, value = model<<6\|yang) | 16-byte `CELL_OBJECT` (absolute int pos, u8 pad/yang, u16 **raw model type**) |
| terrain height | PVS/BSP heightmap from spooled region | per-cell road map @1500 units from spooled `roadh` RLE + `LUMP_ROADSURF` polys |
| pallet lump | 4-int entries `{pal, tex, tpage, clutNum}` | 3-int entries `{pal, tex, tpage}` (clut always inline), count-bounded |
| per-city files | `LEVELS\CITY\CARMODEL_*.MDL/.COS/.DEN`, `.LCF` | none — cars from lump 28, cosmetics exe-embedded |
| roads | STRAIGHTS2/CURVES2/JUNCTIONS2 | ROADS/JUNCTIONS/ROADBOUNDS/JUNCBOUNDS (old formats) |
| roadmap scale | 512 units | 750 units |
| night mode | yes | no (force day) |

D1 **map header** is the same `OUT_CELL_FILE_HEADER` (8 ints + VECTOR_NOPAD)
and the engine's `MAP_REGION_SIZE`/`MAP_CELL_SIZE` macros are header-driven,
so `ProcessMapLump` works as-is — only the straddler copy needs the 16-byte
object conversion.

D1 **spoolinfo** is byte-compatible with D2's parser (`ProcessSpoolInfoLump`
needs no change): verified `model_spool_buffer`, `musamb`, areas, slots_add,
objects_add, pvs sizes, `num_regions` region offsets (140/324/324/256), and
the `Spool` table all parse and the total byte count equals the lump size.

D1 **spooled region data** (per region, offsets in 2048-byte sectors):

```
roadm   RLE of ushort per 1500-unit cell   (surface road ids)  size roadm_size
roadh   RLE of uint per 1500-unit cell     (height/type/angle) size roadh_size
cellPointers   packed (same packtypes 0/1/2 as D2; unpack_cellpointers shared)
cellData       CELL_DATA_D1 (4-byte, linked)                  size cell_data_size[0]
cellObjects    16-byte CELL_OBJECT                            size cell_data_size[2]
(PVS chunk presence unconfirmed — the tool marks it "FIXME: is it even
there in Driver 1?"; we do not read one)
```

D1 cell objects use **absolute world positions** (int32). They convert
cleanly into the engine's packed format: `pos = abs & 0xFFFF` satisfies the
unpack formula `near.x + (short)(ppco->pos.vx - near.x)` for any cell size
(the 65536 wraps cancel), `value = (type << 6) | yang` makes every existing
consumer (draw.c, objcoll.c, camera.c) work unchanged.

## 5. Files touched

Library (Driver 1 logic lives here, engine stays thin):
- `src_rebuild/Game/C/JERICHO/include/jer_d1.h` — SDK-style API
- `src_rebuild/Game/C/jer_d1.c` — game-side implementation

Events (`src_rebuild/Game/C/jer_events.h`, `jericho.h`):
- `JER_EVENT_FRONTEND_SCREEN` — take-a-ride city screen setup/frame
  (button injection)
- `JER_EVENT_LEVELFILE` — `SetCityType` path resolution (rewrite/veto)

Engine gate flag:
- `gDriver1Level` (main.c/main.h) — set from `GameLevel >= 4` in
  `State_GameStart`; every D1 path is gated on it (0 for all D2 play).

Engine hook sites (all thin, JERICHO-HOOK style):
- `main.c` — ProcessLumps D1 lump count; LoadingScreenNames[8]; music default
- `system.c` — LevelNames/LevelFiles[8]; JER_EVENT_LEVELFILE in SetCityType
- `glaunch.c` — set gDriver1Level + force day
- `cars.c` — ProcessPalletLump D1 branch; per-city MDL/COS/DEN guards
- `cosmetic.c`, `denting.c` — per-city file guards
- `spool.c` — LoadRegionData D1 branch; InitSpecSpool guard
- `cell.c` — GetFirstPackedCop/GetNextPackedCop D1 branches (next_ptr chains)
- `map.c` — straddler conversion, D1 PVS fill, roadmap 750 scale
- `dr2roads.c` — FindSurfaceD2 dispatcher to jer_d1_find_surface
- `Frontend/FEmain.c` — D1 city screen slot + Driver1CitySelectScreen +
  JER_EVENT_FRONTEND_SCREEN hook

Mods:
- `mods/driver1/` — take-a-ride "Driver 1 Cities…" page + availability gate
- `mods/levelhacks/` — skip GameLevel >= 4 (one line)

Tests:
- `src_rebuild/tests/d1/` — headless harness (d1test.c + d1stubs.c + bat),
  see §7

## 6. Known limitations (v1)

- **No AI traffic routing**: D1 roads/junctions/bounds are parsed but the
  engine's D2 road tables stay empty, so civ traffic has no routes. Streets
  are drivable/walkable but empty of AI cars. (The old upstream D1 support
  was removed before finishing this.)
- **No night mode** for D1 (force day).
- **D1 car cosmetics** (light positions) are exe-embedded in D1 and there
  are no per-city .LCF files; D1 cars currently use default (zeroed)
  cosmetics — lights may be misplaced. A compat struct (D1 cosmetics ⊂ D2
  struct, policeLight/extra fields zeroed) is the fix.
- **No peds/events**: D1 levels carry no event lump, so nothing spawns
  random peds; the player ped (Tanner, from D2 motion data) works.
- PVS culling disabled for D1 (draw all cells of loaded regions).
- The D1 `SubDivision` lump (20) is a no-op, same as D2 today.

## 7. Test harness (headless)

`src_rebuild/tests/d1/` — a **no-window, no-SDL, no-controller** CLI that
compiles the real `jer_d1.c` against the extracted D1 files and validates
every format assumption. Run from `bin/Release_dev`:

```
..\..\tests\d1\d1test.bat        ->  === ALL PASS (0 failures) ===
```

It verifies the LEV header + citylumps, the D1 lump-count walk, map
headers, byte-exact spoolinfo parsing, region RLE decode (3600 cells per
barrel), cell-object conversion, and `jer_d1_find_surface` (real Miami
street heights, y≈28). This is the primary D1 regression test — no game
launch needed, safe to run while playing anything else.

Known crash bug found by the harness: the per-region scratch buffer's byte
size must live in its own array (`sD1CellObjectBytes`), not in the first
words of the scratch — the spooled cell-object data overwrites them.

## 8. Reference sources

- `OpenDriver2Tools/DriverLevelTool/driver_routines/` — `regions_d1.cpp`
  (D1 region/surface logic), `regions.cpp` (shared spoolinfo + unpack),
  `d2_types.h` (D1 structs: CELL_DATA_D1, CAR_COSMETICS_D1, PALLET_INFO_D1,
  DRIVER1_ROAD/JUNCTION…), `textures.cpp` (D1 pallet)
- REDRIVER2 upstream history: `6ae1468c` "experimental Driver 1 LEV lumps
  support", `2c88991c` "partially figured out Driver 1 spool offsets",
  `f1d79da2` "remove Driver1 level support" (the old gated branches)
- The `DRIVER/` data itself — hex probes are the ground truth
