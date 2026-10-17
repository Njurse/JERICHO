# bankfill — completing sparse car-data banks, and the big rig

Two tools, both offline and both read-only towards the game except where stated:

| tool | what it does |
|---|---|
| [`bankfill.py`](../tools/bankfill.py) | reports on a bank, and **fills** the models it is missing from its host city's own level file, in place |
| [`bigrig_extract.py`](../tools/bigrig_extract.py) | pulls the Chicago big-rig truck out as inspectable assets (OBJ, PNG pages, a decoded cosmetics record) |

## why a bank needs filling

A *bank* is a car-data file the engine registers as a city ([`city-registry.md`](../../../../src_rebuild/Game/C/JERICHO/docs/city-registry.md)).
A retexture pack bakes only the models it repaints — the BetterCars banks in this
install carried 1–2 of 13, with `-1` for the rest — and the engine reads a model as
absent unless **clean, damaged and low are all present and strictly increasing**
(`GetCarImportModels`, `models.c:824-846`). So a pick of any other model from such a
bank fell back to the level's own car, and the roster offered what little it had.

`bankfill.py --fill` copies the missing models out of the host city's
`LEVELS\<CITY>.LEV` — the model records byte for byte, plus the page sets and palette
rows those models need — so the bank then carries the whole host car list.

```
python3 bankfill.py --report --base-dir DRIVER2/LEVELS 'BetterCars/BC*.LEV'
python3 bankfill.py --fill   --base-dir DRIVER2/LEVELS 'BetterCars/BC*.LEV'
python3 bankfill.py --verify --base-dir DRIVER2/LEVELS 'BetterCars/BC*.LEV'
```

`--fill` keeps the original as `<bank>.sparse.bak` the first time it writes.

### the rules, measured rather than assumed

* **The bank's set ids ARE the host city's.** The bake numbered its texture sets from
  the host's lists (measured: BCCHI and CHICAGO share 50/58/62/63/65), so a host
  model's `texture_set` bytes are already meaningful in the bank — and where the bank
  ships that set, **the bank's page is the repaint** and is kept. No poly stream is
  ever re-encoded: every copied model is the host's own bytes.
* **Only the sets the bank lacks are appended**, with the host's page bytes, and the
  file is rebuilt so the page data still starts at `pageBase`
  (`(data1Off/2048 + data1Size/2048)*2048`, `models.c:393`).
* **The permanent page list is a fixed 16-slot region** (`texture.c:1220` reaches the
  special list through `&permlist[16]`), so appended sets go there while they fit and
  on the special list after that; the tool says which.
* **A set neither city lists is an id convention, not a gap.** Host models name ids
  like 253/254/255 (and 0 in VEGAS) that no page list carries in either city; the
  copies keep them and the engine leaves those polys to the page's own CLUT, exactly
  as it does for the host's own car. This is the "the rest name a set the import has
  no page for" line in a page check, and it is the same shortfall the host's car has.
* **Palette records are copied byte for byte** (inline CLUTs included) for the sets
  that are added; a set the host keeps no rows for stays rowless, which is normal.

What the fill measured here (all four banks, `--verify` clean):

| bank | carried before | after | sets appended | palette records |
|---|---|---|---|---|
| BCCHI | 10, 11 | all 9 host models | 1 | +30 |
| BCHAV | none | all 9 | 0 | +0 (host keeps no rows for it) |
| BCRIO | 0 | 10 | 1, 2 | +0 |
| BCVGS | none | all 9 | 1, 2 | +0 |

In the engine, importing a filled model builds it and pins its page:
`slot 5 geometry from BCCHI model 2` and
`page check - slot 5 clean: 228 of 244 polys resolve to a pinned imported page`, with
the roster going from `offering the 2 car(s)` to `offering the 9 car(s)`.

## the big rig: where the truck actually comes from

**The Caine's Compound semi is not in the level.** The seven `multiCar` events at
`event.c:1160` all name the scenery model `"LORRY"`, and in every Chicago level this
install ships (`LEVELS`, `NLEVELS`, the untouched `DRIVER2 original - Copy`) that name
resolves to model slot **1196, whose pack size is 0** — an empty slot. Havana, Vegas
and Rio have no `LORRY` at all.

**The big rig is CAR model 11, and the level does not ship it either.**
`levmodels.py` shows model 11 as a gap in Chicago's `LUMP_CAR_MODELS`, which is why
`FEmain.c:3068` gates Chicago's slot 8 on
`gContentOverride && FileExists("LEVELS\CHICAGO\CARMODEL_11_clean.MDL")` — commented
"remove truck". Its geometry lives in the loose override files, which is what
BetterCars supplies and what `bigrig_extract.py` reads.

Measured (the installed BetterCars truck):

| | clean | low |
|---|---|---|
| vertices / polys | 126 / 204 | 120 / 134 |
| extent (x, y, z) | 402 × 464 × **2208** | 390 × 464 × 1823 |
| texture sets | 1, 50, 68, 69 | 1, 68, 69 |

A stock Chicago car is 414 × 338 × 1306 (`model 10`), so the truck is ~1.7× as long
and ~1.4× as tall — the numbers to start a handling profile from.

Its `CARMODEL_11.COS` decodes to a full `CAR_COSMETICS` record: `wheelDisp` at
x ±200 / z ±305, `wheelSize` 67, `mass` 4096, `colBox` 193 × 130 × 467, `cog`
(0, -260, 0), `powerRatio`/`susCoeff`/`traction` all 4096 (1.0). That is the record a
`BIGRIG.LCF` would carry — nothing here invents handling numbers.

```
python3 bigrig_extract.py            # default --out: out/bigrig
```

writes `truck_clean.obj`, `truck_low.obj`, `truck.mtl`, `pages/SET_{1,50,68,69}.png`
(each page decoded with palette row 0 — for looking at the art, not a claim about
in-game colours) and `truck.json` with both geometries and the decoded cosmetics.

### what is NOT done

The bank itself: `BIGRIG.LEV` + `BIGRIG.LCF` and its handling profile are deliberately
left for a follow-up, so the numbers above can be judged first. When it is baked, the
truck wants model **1** (carhacks treats model 0 as "no car"), clean geometry repeated
for damaged and low, and the four sets above ripped from `CHICAGO.LEV` — which is
exactly what `bankfill.py`'s page/palette machinery already does.
