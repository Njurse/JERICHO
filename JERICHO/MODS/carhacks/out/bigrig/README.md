# Chicago big rig - extracted assets

Produced by `JERICHO/MODS/carhacks/tools/bigrig_extract.py` from the installed
BetterCars files. **Nothing here has been baked into a bank yet** - this is the
material to judge before a `BIGRIG` bank and its handling profile are made.

## where the truck actually is

* The Caine's Compound trucks are seven `multiCar` events naming the scenery model
  `"LORRY"` (`event.c:1160`). In every Chicago level this install ships that name
  resolves to model slot **1196 with pack size 0** - an empty slot. There is no
  LORRY geometry to rip. (Havana/Vegas/Rio have no LORRY at all.)
* The big rig is **car model 11**. The level does not ship it either: it is a gap in
  Chicago's `LUMP_CAR_MODELS`, and `FEmain.c:3068` gates Chicago's slot 8 on
  `gContentOverride && FileExists("LEVELS\CHICAGO\CARMODEL_11_clean.MDL")`
  ("remove truck"). BetterCars supplies that override, and it is what was extracted.

## files

| file | what |
|---|---|
| `truck_clean.obj`, `truck_low.obj` | geometry with UVs, one group + material per texture set |
| `truck.mtl` | one material per set, pointing at `pages/SET_*.png` |
| `pages/SET_{1,50,68,69}.png` | the four pages the truck names, decoded with palette row 0. For looking at the art - in game the colour comes from the car's `civ_clut` row, not the page's own CLUT |
| `truck.json` | both geometries' stats, the sets, and the `CAR_COSMETICS` record decoded field by field |

## the numbers (from `truck.json`)

| | clean | low |
|---|---|---|
| vertices / polys | 126 / 204 | 120 / 134 |
| extent x, y, z | 402 x 464 x **2208** | 390 x 464 x 1823 |
| texture sets | 1, 50, 68, 69 | 1, 68, 69 |

A stock Chicago car (model 10) is 414 x 338 x 1306, so the truck is ~1.7x as long and
~1.4x as tall.

Cosmetics from `CARMODEL_11.COS` (a full 236-byte `CAR_COSMETICS`):

```
wheelDisp  [+200,-30,+298] [+199,-30,-313] [-201,-30,+298] [-200,-30,-313]
wheelSize  67          mass       4096
colBox     [193,130,467]           cog  [0,-260,0]
powerRatio 4096        susCoeff   4096        traction 4096
headLight  [142,140,-397]          exhaust [177,240,-50]
```

(4096 is the engine's ONE, i.e. 1.0.) These are the driver's own numbers - nothing
here invents handling.

## open questions for the handling profile

* `mass`/`powerRatio`/`suspension` are the truck's as shipped for Chicago model 11.
  Whether BIGRIG should feel heavier than that is a taste call.
* The bank wants the truck at model **1** (carhacks reads model 0 as "no car"), with
  clean geometry repeated for damaged and low.
* The four sets above must be ripped from `CHICAGO.LEV` into the bank's page list -
  `bankfill.py` already does exactly that for the fill work, so the bake can reuse it.
