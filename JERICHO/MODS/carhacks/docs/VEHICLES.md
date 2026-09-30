# Vehicle reference (model numbers -> what they are)

The point of this file: stop re-deriving "which model number is the fire truck".
A car is identified by **(city, model number)**; the same number is a *different*
vehicle in each city. Regenerate the data columns with:

```
python3 tools/levmodels.py DRIVER2/LEVELS/CHICAGO.LEV DRIVER2/LEVELS/HAVANA.LEV \
                           DRIVER2/LEVELS/VEGAS.LEV   DRIVER2/LEVELS/RIO.LEV
```

## The numbering scheme

- **Model numbers 0..12.** 0..4 are the civilian cars, 5/6/7 are a **gap in every
  city**, 8..12 are the special bodies (fire truck, police, bus, ...). The game also
  has a pseudo-model **13** = "derived civilian" (`10 - (m0+m1+m2)`).
- **Frontend slots 1..10** map to models through `carNumLookup`
  (`FEmain.c:626`) = `1, 2, 3, 4, 0, 8, 9, 10, 11, 12`. So `-car slotN` is a *frontend*
  slot: `-car slot5` = model **0**, `-car slot6` = model 8, `-car slot7` = model 9.
- **`-car <n>`** takes a model number directly. This is the one to use: it is not
  offset by the specials.
- **Resident slots 0..7** are what the level actually holds: 0..4 = the mission
  header's civilian models, 5..6 = spare (modules), 7 = `SPECIAL_CAR_SLOT` (the cop /
  special body). `import = <slot>:<city>:<model>` writes a foreign model into a
  resident slot; `ap.model` on a live car is a **resident slot**, not a model number
  (so `JERICHO-RUN` prints `carslot=` and `model=` separately).

## This install: which models exist where

`yes` = the city ships clean + damaged + low for that model. `--` = absent;
forcing one leaves NULL model pointers and the `CreateDentableCar` guard fires.

| model | CHICAGO | HAVANA | VEGAS | RIO | kind | pages (per city, `carTpages`/`specTpages`) |
|---|---|---|---|---|---|---|
| 0 | yes | yes | yes | yes | civilian | civ slot 0 |
| 1 | yes | yes | yes | yes | civilian | civ slot 1 |
| 2 | yes | yes | yes | yes | civilian | civ slot 2 |
| 3 | yes | yes | yes | yes | civilian | civ slot 3 |
| 4 | yes | yes | yes | yes | civilian | civ slot 4 |
| 5 | -- | -- | -- | -- | gap | — |
| 6 | -- | -- | -- | -- | gap | — |
| 7 | -- | -- | -- | -- | gap | — |
| 8 | yes | yes | yes | yes | special | CHI 54/55 · HAV 38/39 · VEG 18/19 · RIO 66/67 |
| 9 | yes | yes | yes | yes | special | CHI 66/67 · HAV 38/39 · VEG 65/66 · RIO 77/78 |
| 10 | yes | yes | yes | yes | special | CHI 56/57 · HAV 42/43 · VEG 67/68 · RIO 73/74 |
| 11 | **--** | yes | yes | yes | special | (no data in Chicago) CHI 68/69 · HAV 44/45 · VEG 11/12 · RIO 75/76 |
| 12 | yes | yes | yes | yes | special | CHI 61/64 · HAV 48/49 · VEG 63/64 · RIO 69/70 |

Civilian `carTpages` (the 6 sets each city's civ cars paint with):

| city | carTpages[0..5] |
|---|---|
| CHICAGO | 1, 58, 65, 62, 50, 63 |
| HAVANA  | 10, 36, 35, 20, 37, 51 |
| VEGAS   | 41, 59, 54, 62, 17, 32 |
| RIO     | 55, 59, 57, 68, 58, 60 |

## Names (fill in as identified)

Known so far, and worth keeping:

| city | model | name | note |
|---|---|---|---|
| CHICAGO | 8 | fire truck | `-car slot6` |
| CHICAGO | 10 | school bus | `CARMODEL_10_clean.dmodel`; `-car slot8` |
| CHICAGO | 11 | (reserved, no data) | forcing it crashes in load |
| VEGAS | (special) | ambulance | "Steal the Ambulance" mission; exact model TBC |
| HAVANA | **0** | **police car** | found by cycling (`tools/cycle_vehicles.bat`); drives from `import = 3:1:0` in Rio (slot 3 = Rio's model 0) |
| HAVANA | 9 | truck | seen in Rio (import slot 5) — so 9 is NOT the cop car |

Not yet identified — look at the car in a level, then write it here:

| city | model | name |
|---|---|---|
| CHICAGO | 0 | ? |
| CHICAGO | 1 | ? |
| CHICAGO | 2 | ? |
| CHICAGO | 3 | ? |
| CHICAGO | 4 | ? |
| CHICAGO | 9 | ? |
| CHICAGO | 12 | ? |
| HAVANA | 0 | police car (identified) |
| HAVANA | 1..4 | ? |
| HAVANA | 8 | ? |
| HAVANA | 10 | ? |
| HAVANA | 11 | ? |
| HAVANA | 12 | ? |
| VEGAS | 0..4 | ? |
| VEGAS | 8..12 | ? |
| RIO | 0..4 | ? |
| RIO | 8..12 | ? |

(The quickest way to look: `import = 5:<city>:<model>` + `-car <model>` in a level
you know, or just drive with `-car <model>` on that city's own level.)

## Related

`CROSS_CITY.md` (how imports work) and `HACK.md` (the debugging train of thought).
