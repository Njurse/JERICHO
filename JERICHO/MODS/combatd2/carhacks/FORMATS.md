# The proprietary formats, as far as we've cracked them

Everything here was measured off this install's data and cross-checked against
the engine's own runtime logs (`LUMP_CAR_MODELS: size: …` reproduced exactly).
Line references are into `src_rebuild/Game/`. Where a number is stated, it was
measured, not guessed.

Nothing in Driver 2 comes with a spec, so this file is deliberately written as
"what the bytes are" + "how to re-check it yourself" (recipes at the end).

---

## 1. A city's level file: `LEVELS\<CITY>.LEV`

(Multiplayer arenas use `MLEVELS\`, the third variant `MNLEVELS\`.)

```
offset 0   : LUMP header      type (4 bytes) + size (4 bytes)   <- skipped, hardcoded in the engine
offset 8   : citylumps[8][4]  four XYPAIRs = eight int32
offset N   : DATA1  ......... this is where the car models and car palettes live
             TPAGE  ......... texture pages + CLUTs (uploaded to VRAM)
             DATA2  ......... the general level data
             SPOOL  ......... streamed-on-demand data
```

`citylumps` is read with a plain `fread` from the level file (`system.c:~917`);
`loadsectorsPC` (`system.c:535`) later re-reads by `fseek(sector * CDSECTOR_SIZE)`
on the same path, so `.x` is a **byte** offset and `.y` a **byte** size.

| Index | Name | `system.h` |
|---|---|---|
| 0 | DATA1 | `CITYLUMP_DATA1` |
| 1 | TPAGE | `CITYLUMP_TPAGE` |
| 2 | DATA2 | `CITYLUMP_DATA2` |
| 3 | SPOOL | `CITYLUMP_SPOOL` |

Measured sizes (bytes):

| City | file | DATA1 | TPAGE | DATA2 | SPOOL |
|---|---|---|---|---|---|
| CHICAGO | 10915840 | 217088 | 219136→565248 | 784384→256000 | 1040384→8214528 |
| HAVANA | 10915840 | 208896 | — | — | — |
| RIO | 10915840 | 215040 | — | — | — |
| VEGAS | 10915840 | 221184 | — | — | — |

(Every `.LEV` is exactly 10915840 bytes = 5330 sectors, i.e. fixed-size padded
containers. The first three of the file, in hex, for CHICAGO:
`25 00 00 00 f8 07 00 00 | 00 08 00 00 | 00 50 03 00 | …` — type 0x25 = 37, then
the size, then `citylumps[0] = 2048` (byte 8) and `citylumps[1] = 217088`.)

### DATA1 and DATA2 are themselves containers

The `citylumps` entry points at a lump, **not** at the segment list. DATA1's body
is `LUMP_LEVELDESC` (35) and DATA2's is `LUMP_LEVELDATA` (36). The engine passes
`ptr + 8` to `ProcessLumps` (`main.c:422`, `main.c:439`), i.e. it skips that
container's own type+size header first.

### Walking segments — the one detail that bites

`ProcessLumps` (`main.c:190`) reads `type = *(int*)ptr`, `size = *(int*)(ptr + 4)`,
body at `ptr + 8`, and advances:

```c
lump_ptr = (char*)ptr + ((seg_size + 3) & ~0x3);   // main.c:378
```

**Segments are 4-byte aligned.** Walking `size + 8` looks right and works until a
segment has a size that isn't a multiple of four — a 2163-byte segment knocked a
naive walk into garbage on its fourth step. Any tool (or engine code) reading a
second city's file must align the same way.

### Segment ids seen in DATA1

| Id | Name | Notes |
|---|---|---|
| 1 | `LUMP_MODELS` | world/level models |
| 5 | — | |
| 12 | — | |
| 25 | `LUMP_PALLET` | **the car palettes** (see §4) — not texture data, despite the name |
| 28 | `LUMP_CAR_MODELS` | **the car geometry** (see §3) |
| 34 | — | |
| 35 | `LUMP_LEVELDESC` | DATA1's own container type |
| 36 | `LUMP_LEVELDATA` | DATA2's own container type |

---

## 2. Texture sets: the naming scheme that ties it together

Before the tables, the concept, because it explains every "wrong colours" bug.

A car polygon (a `POLYGT3`/`POLYGT4`) carries a **`texture_set`** and a
**`texture_id`**. Those are not addresses — they are indices into two arrays the
engine fills while a level loads:

```c
u_short texture_pages[128];        // texture.c:94  set index -> VRAM tpage handle
u_short texture_cluts[128][32];    // texture.c:95  set index -> CLUT handle
```

`LoadPermanentTPages` (`texture.c:475`) starts by setting **every** entry to a
dummy — `texture_pages[i] = GetTPage(0,0,960,0)` and
`texture_cluts[i][j] = GetClut(960,16)` — and then fills in only the sets the
current level actually loads (`LoadTPageAndCluts`, `texture.c:275`, which
registers `texture_pages[set]` and `texture_cluts[set][i]`, and uploads the page
to VRAM through `loadsectors` from the **current** level file).

Consequence, and the reason foreign cars render wrong: any `texture_set` the
level did not load resolves to tpage `(960,0)` / CLUT `(960,16)`. A car whose
polys point there draws nothing usable — **invisible** — or draws with the dummy
palette. This is precisely what a cross-city vehicle hits.

**Per-city set assignments** live in `char carTpages[4][8]` (`texture.c:70`):
eight texture-set numbers per city, filled in partly statically and partly at
runtime (`spool.c:1719`, `texture.c:555`). `GetCarPalIndex` (`cars.c:1876`) is
nothing more than "which of this city's eight entries equals this `texture_set`".

Measured car-page sets (from each city's `LUMP_PALLET` `tpageindex` field):

| City | car texture sets |
|---|---|
| CHICAGO | 1, 50, 62, 63, 65 |
| HAVANA | 10, 20, 35, 37, 51 |
| RIO | 55, 57, 58, 60, 68 |
| VEGAS | 17, 32, 41, 54, 62 |

Effectively disjoint — only 62 is shared (CHICAGO/VEGAS). Every city renumbers
its own art into the same eight slots, so a set number means different things in
different cities.

---

## 3. `LUMP_CAR_MODELS` (id 28) — a city's car geometry

```
+0                   : header (skipped; the engine starts reading at +4)
+4                   : offset table — THREE int32 per model, models 0..12
                       so model 12's table occupies +4+144 .. +160
+4+160 = +164        : the data area; all offsets above are relative to THIS
```

Each model's three offsets are `clean`, `damaged`, `low detail`. `-1` means "this
city has no such variant". The engine indexes the table as
`lump_ptr + 4 + model * sizeof(int) * 3` and reads the geometry at
`models_offset + offset` where `models_offset = lump_ptr + 4 + 160`
(`models.c:ProcessCarModelLump`), then hands it to `GetCarModel` +
`buildNewCarFromModel`, which **copy into the level heap** (`mallocptr`) — which
is why an import only needs the source bytes transiently.

Measured: block size CHICAGO 123324 / HAVANA 131412 / RIO 135688 / VEGAS 132252
bytes. Which models exist:

| Model | CHICAGO | HAVANA | RIO | VEGAS |
|---|---|---|---|---|
| 0-4 | yes | yes | yes | yes |
| 5, 6, 7 | **no** | **no** | **no** | **no** |
| 8-10 | yes | yes | yes | yes |
| 11 | **no** | yes | yes | yes |
| 12 | yes | yes | yes | yes |

So models 5/6/7 are gaps everywhere, and `-car slot9` (model 11) kills a
**Chicago** level during load while being a real vehicle in the other three.
Requiring clean **and** damaged **and** low before using a model matters: a slot
holding a partially-present model is left with `NULL` model pointers, which is
the state `CreateDentableCar` complains about and then dereferences.

Within a model, `MODEL`'s first fields are used for sizing: `poly_block` (the
polygon data) and `normals`, e.g.
`size = ((MODEL*)(models_offset + cleanOfs))->poly_block`.

---

## 4. `LUMP_PALLET` (id 25) — the car palettes

Consumed by `ProcessPalletLump` (**`cars.c:1427`**, not `texture.c` — the generic
name misleads), which merges it into:

```c
u_short civ_clut[8][32][6];   // cars.c:97   [car palette][texture id][colour slot]
```

Layout:

```
+0   : int32 total_cluts   (0 = nothing to do)
+4   : records, each FOUR int32 until a -1 is hit:
         palette        (0..5, the colour slot within the palette)
         texnum         (the texture id)
         tpageindex     (the texture SET number — see §2)
         clut_number    (-1 = the CLUT image follows inline; else reuse an earlier one)
       if clut_number == -1:
         +16 : the CLUT image, 8 int32 (32 bytes)
         ...  record is 16 + 32 = 48 bytes
       else:
         ... record is 16 bytes
```

The engine's write is
`civ_clut[GetCarPalIndex(tpageindex)][texnum][palette + 1] = clutValue`
(`cars.c:1472`) — note `palette + 1`, slot 0 being reserved for the base colour
that the draw path fills per-frame from `texture_cluts` (`cars.c:1263`, `:1283`).

Measured: 15576 bytes / 219 cluts (CHICAGO), 14792 / 192 (HAVANA),
15704 / 228 (RIO), 14808 / 200 (VEGAS).

---

## 5. `LEVELS\<CITY>.LCF` — per-model car cosmetics

`LoadCosmetics` (`cosmetic.c:93`) reads a fixed **3120 bytes** of
`CosmeticFiles[level]` (`cosmetic.c:13`) and hands it to `ProcessCosmeticsLump`,
which per resident slot does:

```c
offset = *(int*)(lump_ptr + model * sizeof(int));
car_cosmetics[i] = *(CAR_COSMETICS*)((u_char*)lump_ptr + offset);
```

So: an int32 offset table indexed by model number, then `CAR_COSMETICS` records —
the same shape as the car-models block. `CAR_COSMETICS` is what the physics and
lighting read too: `->mass`, `->cog` (centre of gravity, `cars.c:1511`) and
`->colBox` (the collision box, `bcollide.c:410`, `:549`). An imported vehicle
therefore needs this file as well, or it keeps the host's weight and box.

Gotcha: the special slot does **not** read it per model — it takes a cached
`levelSpecCosmetics[model - 8]` filled for models 8..12 (`cosmetic.c:140-160`).

---

## 6. The car draw path, end to end

```
model geometry (LUMP_CAR_MODELS)
  -> GetCarModel / buildNewCarFromModel   copy into the level heap
  -> prims: POLYGT3 / POLYGT4 with texture_set + texture_id
  -> clut_uv0 = M_INT_2(texture_cluts[texture_set][texture_id], uv)     cars.c:1225, :1239
     tpage_uv1 = M_INT_2(texture_pages[texture_set], uv)
  -> every frame: civ_clut[carid][texture_id][0] = texture_cluts[...]   cars.c:1263, :1283
  -> draw: pg->pciv_clut[(clut_uv0 >> 0x10) + palette]                  cars.c:275, :1002
```

`palette` here is the car's colour choice (from `CAR_COSMETICS` / the chosen
palette), and it indexes the six slots of `civ_clut[carpal][texid][…]`.

Read together with §2 this is why a cross-city car needs *three* things from the
other city: its **geometry** (28), its **palettes** (25) and the **texture pages**
its sets point at (the `TPAGE` region + the level's spool page list).

Colour words anywhere in this engine are packed **`B << 16 | G << 8 | R`** — red
in the low byte (the code is PSX-derived; `DrawCarObject` packs tints that way).

---

## 7. Recipes: measuring this yourself

All of these are read-only. Run from `src_rebuild/bin/Release_dev/DRIVER2`.

Dump the citylumps table and walk DATA1/DATA2 segments:

```python
import struct
blob = open('LEVELS/CHICAGO.LEV','rb').read()
tbl = struct.unpack_from('<8i', blob, 8)          # DATA1.x, .y, TPAGE…, DATA2…, SPOOL…
def segs(buf, off):
    while off + 8 <= len(buf):
        t, s = struct.unpack_from('<ii', buf, off)
        if s < 0 or off + 8 + s > len(buf): break
        print('  type', t, 'size', s)
        off = off + 8 + ((s + 3) & ~3)            # 4-byte aligned, as main.c:378
segs(blob[tbl[0]:tbl[0]+tbl[1]], 8)               # DATA1 body (skip its container header)
```

Find the car-models block and report each model's three offsets:

```python
# locate type 28 in DATA1 as above, then:
off, size = block_offset, block_size
for m in range(13):
    print(m, struct.unpack_from('<iii', blob, off + 4 + m*12))
```

Count a city's car palettes and which sets they use:

```python
# locate type 25, then:
total = struct.unpack_from('<i', blob, off)[0]
q, sets = off + 4, {}
while True:
    palette, texnum, tpage, clutnum = struct.unpack_from('<iiii', blob, q)
    if palette == -1: break
    sets.setdefault(tpage, set()).add(palette)
    q += 16 + (32 if clutnum == -1 else 0)
print(total, sets)
```

---

## 8. Gotchas, collected

- Segments are **4-byte aligned** on the way in; a raw `size + 8` walk drifts.
- DATA1/DATA2 entries point at a **container**, so their own 8-byte header is
  skipped before the segment list starts.
- `LUMP_PALLET` = car palettes (`cars.c`), *not* texture palettes.
- Colour words are `B<<16 | G<<8 | R`.
- A `texture_set` means a different page in every city; unknown sets resolve to
  the dummy tpage `(960,0)` / CLUT `(960,16)`, which renders as nothing.
- Models 5/6/7 are absent in **every** city; Chicago's 11 is absent too.
- `REDRIVER2.log` is **truncated at session start and flushed at close** — a
  `taskkill` throws the whole session away, and a stale line-count boundary reads
  nothing. Wait for `---- LOG CLOSED ----` before believing a log.
