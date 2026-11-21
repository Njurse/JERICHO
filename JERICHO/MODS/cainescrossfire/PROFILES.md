# Vehicle profiles

The vehicle profile system is the roster of the game: one **profile** per
contestant's car, holding everything that makes that car *that car* — its
identity and home arena, the exact vehicle it uses in a stock city's or
carhacks pack's asset files, its 1..5 design stats, its unique special weapon,
and the physics it overrides.

It lives in `profiles/`:

| File | Role |
|---|---|
| `*.json` | **the data** — one file per profile |
| `profile.h` | the runtime row (`CD2_VEH_PROFILE`) and the registry API |
| `generated_profiles.c` | **generated** from the JSON by `tools/gen_profiles.py` — do not edit |
| `registry.c` | resolves names, cities/banks and specials; the accessors |
| `profiles_map.c` | resolves profiles into the engine (resident slots, cosmetics, paint) |
| `tools/gen_profiles.py` | validates the JSON and writes `generated_profiles.c`; `--check` for CI |
| `tools/packprofile.py` | scaffolds one JSON per car from a carhacks pack's `bank.json` |

## Adding a profile

1. Add `profiles/<name>.json` (copy an existing file). `internal` must be unique;
   `order` is optional and only decides roster order (the original files keep
   0..10, so their indices do not move).
2. If the special is new, add its def under `weapons/special/` and its id to
   `weapons/core/weapon.h`; otherwise name an existing `special_<car>`.
3. Build. `JERICHO/build.py` runs `gen_profiles.py` before compiling the module;
   by hand it is
   `python JERICHO/MODS/cainescrossfire/tools/gen_profiles.py`.

A car pack can be turned into starter profiles with:

```sh
python JERICHO/MODS/cainescrossfire/tools/packprofile.py \
    --bank JERICHO/MODS/cainescrossfire/vehicles \
    --out JERICHO/MODS/cainescrossfire/profiles
```

## The profile file

```json
{
  "order": 0,
  "internal": "hornet",
  "display": "Hornet",
  "homeCity": "CHICAGO",
  "vehicle": {
    "bank": "CROSSFIRE",
    "model": 2,
    "lev": "..\\JERICHO\\MODS\\cainescrossfire\\vehicles\\build\\CROSSFIRE.LEV",
    "lcf": "..\\JERICHO\\MODS\\cainescrossfire\\vehicles\\build\\CROSSFIRE.LCF"
  },
  "stats": { "armor": 2, "speed": 4, "handling": 4, "specialPower": 3 },
  "special": "special_hornet",
  "physics": { "mass": 3000, "powerRatio": 4400, "topSpeedPct": 115 },
  "palette": 3,
  "crew": { "lat": [0, 0], "fwd": [0, 0], "up": [0, 0], "yaw": [0, 0], "sillRaise": 0, "armScale": 0 }
}
```

- **Identity.** `internal` is code-facing (`hornet`); `display` is on screen
  (`Hornet`); `homeCity` is the stock arena the contestant belongs to and the
  city the "field the home roster" rule reads. It does **not** have to be the
  vehicle's source.
- **Vehicle source.** `vehicle` is `{ "city": "CHICAGO", "model": 3 }` for a
  stock car, or `{ "bank": "CROSSFIRE", "model": 2 }` for a carhacks pack. The
  model is the number in that source's `CARMODEL_<n>` set (0..12, see
  `carhacks/docs/VEHICLES.md`). A bank may carry `lev`/`lcf` paths in the same
  spelling `cities.ini` uses (relative to the game's data folder); the module
  registers the bank from them when carhacks/cities.ini have not.
- **Core stats.** Armor / Speed / Handling / Special Power, each 1..5. These are
  the design + HUD numbers; Speed and Handling also shape the sim (below).
- **Special.** The weapon's code name (`special_hornet`), or `"none"`. The
  weapon owns its display name, ammo and recharge.
- **Physics overrides.** Per-field substitutes for the vehicle's `CAR_COSMETICS`
  (`mass`, `powerRatio`, `traction`, `susCoeff`, `wheelSize`, `twistRateX/Y/Z`,
  `cogY`, plus a sim-level `topSpeedPct`). **A missing field (or 0) means "keep
  the model's own value"** — so a profile can be heavy on one axis and still
  pull the rest from the original model. The override is written into
  `car_cosmetics[slot]` at level start, before any car is built.

## Crew offsets (`CD2_VEH_CREW`)

The mounted crew (the driver and passenger who lean out to fire — see
[`MOUNTED_CREW.md`](MOUNTED_CREW.md)) hang on the door with a default mount. A
body whose doors sit somewhere the default does not fit — a limo, a truck, a very
wide low car — can move it per profile:

```json
"crew": { "lat": [6, 6], "fwd": [0, 0], "up": [0, 0], "yaw": [0, 0], "sillRaise": 0, "armScale": 0 }
```

- **Sides** are indexed `{ driver, gunner }` (left door, right door).
- **`lat` / `fwd` / `up`** are world-unit deltas on the mount point: further out
  from the door, further ahead of the cabin centre, higher on the body (the
  render frame is Y-down). **`yaw`** turns the body (a window that is not square
  to the car). **`sillRaise`** perches the seated passenger higher.
  **`armScale`** is the weapon arm's reach as a per-cent of the default (`0` =
  100%).
- Every field is a **delta** and a missing field is 0, which leaves the default
  alone; the `crew` object is optional, so a profile only states what it has to
  change.
- Consumption is in `weapons/core/crew.c` (`cd2CrewOffsets`, applied in
  `cd2CrewPlace` and the arm pose). The exact look still needs a play-test.

The boot dump prints each profile's offsets, so a run shows which bodies carry an
override:

```
[cainescrossfire]       crew offsets lat{6,6} fwd{0,0} up{0,0} yaw{0,0} sill 0 arm 100%
```

## Where a profile meets the engine

Timing (verified in `main.c`): `SetupResidentModels` fires
`JER_EVENT_CAR_DATA_SOURCE`, then `LoadGameLevel → LoadCosmetics` fills
`car_cosmetics[]`, **then** `JER_EVENT_GAME_START`, and only after that are the
cars created. So:

1. **CAR_DATA_SOURCE** — each *fielded* profile's vehicle source
   `(city/bank, model)` is placed into a resident car slot:
   - a **native** profile (its vehicle source is the level's) reuses a resident
     slot that already holds its model; failing that it takes an empty **spare**
     slot, the level's own lump supplying the geometry;
   - a **foreign** profile is placed into an empty spare slot with **its own
     source city or bank** recorded as that slot's model source, so the engine
     reads the model out of that source's own car data rather than the level's.
     There is no one-guest-city limit: `models.c` keeps a `CAR_IMPORT` **per
     city** (`gCarImports[4]`) and `InitCarImport` loads every source the slots
     ask for. Measured (2026-10): one Rio match fielded seven cars spanning all
     four cities, with the engine logging three guest imports at once — CHICAGO
     + VEGAS + HAVANA beside Rio's own. A profile whose `vehicle.bank` is not
     registered is skipped with a log line; `homeCity` plays no part in this
     choice.
   Civilian slots (0..4) are never repurposed — they carry the level's own models
     and the ambient traffic, and stealing one is what made the level's cars look
     wrong.
2. **GAME_START** — the profile's `CAR_COSMETICS` overrides are written.
3. **CAR_STEP** — the first time a car of a profiled slot is seen, it is assigned
   its profile, handed its special (filled to capacity, per-car ammo), and given
   the profile's paint (`cp->ap.palette`).

`cd2GetStats` (`cainescrossfire.c`) then layers the profile's **Speed** and
**Handling** stats onto the derived handling (a ~0.80x..1.20x multiplier, 3 =
stock) plus `topSpeedPct`, and the **Armor** stat scales the damage the car takes
on both the scenery and car-vs-car paths.

### Which profiles are fielded

- the `[cainescrossfire] profiles` list (internal names, comma separated), or —
  when empty — the profiles whose `homeCity` is the level's own;
- plus the profile the **player** picked in the CC select flow;
- `CC_PROFILES` overrides the whole set for a headless run (never persisted).

`homeCity` is identity only: a Chicago contestant can drive a CROSSFIRE-bank
car and is still fielded on Chicago when the list is empty.

## What the engine affords

This is the one place that states the capacity a battleground has for distinct cars. It
is not a CC setting — it is the engine's, and carhacks' `docs/CROSS_CITY.md` covers the
VRAM side of it in depth. Everywhere else that needs a number should point here.

**Resident car slots: `MAX_CAR_RESIDENT_MODELS` = 16** (`src_rebuild/Game/dr2limits.h`),
with `SPECIAL_CAR_SLOT` **pinned at 11** — not `MAX_CAR_RESIDENT_MODELS - 1` any more. It
is a MODEL number compared against `cp->ap.model` (the Vegas ambulance, mission 33's limo)
and names the slot the special model is spooled into, so it must not follow a capacity
change: it was 11 only because the pool used to be 12 long. Slots above it are spare.

| slots | who holds them |
|---|---|
| 0..4 | the level's own civilian models — **never repurposed** (stealing one is what made the level's cars look wrong) |
| 5..10, 12..15 | free: the module's budget — **ten** cars (mp claims 5/6; carhacks imports into 7..10 + 12..15) |
| 11 | the special vehicle (`SPECIAL_CAR_SLOT`) |

A native profile does not need a free slot at all: it reuses the resident slot that
already holds its model number. So an arena can field the level's own cars (in the
civilian slots they already occupy) **plus ten more**, and a roster car whose model is
already resident costs nothing.

**Guest cities: 15** palette bands (128-row CIV_CLUT); in practice 3 — that is, every city that can be one (there are four, and the
level's own is not a guest). `models.c` keeps one `CAR_IMPORT` per city (`gCarImports[4]`,
`.region == NULL` when not held) and `InitCarImport` loads every city the placed slots ask
for. A foreign profile is placed into a free slot with **its own city** recorded as that
slot's model source, and the geometry comes out of that city's own file.

**Guest city palettes: one 8-row civic CLUT block each.** Rows `0..7` are the level's own;
a guest gets the next free band — measured, CHICAGO `8..15`, HAVANA `16..23`, VEGAS
`24..31`. The derivation, the `CIV_CLUT_GUEST_CITIES` arithmetic and the VRAM reasoning
live in carhacks' `docs/CROSS_CITY.md`, which owns the palette side (the tables sit in the
lower half pool, so the level font at rows 466..511 is never written) — CC depends on
carhacks, so the pointer runs that way and the number is not restated here. If a city is
ever refused, `cars.c` says so out loud: `NO PALETTE BLOCK (refused; civ_clut affords N
guests...)`.

**What the pool costs is per model LOADED, not per slot.** Growing the pool 8 → 12 changed
neither the texture budget (`JERICHO-VRAM` reports the same 608 KiB of car texture slots,
same 704/1408 KiB total, no overflow) nor the level heap; only the car poly budget scales
with the count (3200 → 4800, where a loaded level uses 1900–2700). The capacity is
allocated when a module claims a slot, so stock levels are unchanged.

**Measured end to end:** one Rio match, the player on Hornet and six contestants, fielded
**seven cars spanning all four cities** — hornet (slot 10, imported CHICAGO), avalanche
(9, imported VEGAS), highwayman (8, imported HAVANA), bootlegger (7, imported VEGAS),
fixer (6, imported CHICAGO), corvo and invocada (RIO's own) — with the engine logging
three guest imports at once and nothing unfielded.

**The failure mode to recognise.** A profile that cannot be placed logs

```
[cainescrossfire] profile obelisk: no free spare slot for RIO model 11, not fielded
```

and that car then ships as **the level's own model at that slot number** — no profile, no
stat overrides, no special weapon. Anything that shrinks the field or steals a slot shows
up this way, and it is worth checking for it whenever a car "arrives without its special":

- more cars than the pool can hold: the six free slots (5..10) are what cars that are
  *not* already resident draw from, so a match wanting several foreign cars at once is
  where one loses — and the one that loses degrades into its domestic counterpart. The
  roster is larger than any single battleground can hold, so this is a real ceiling, not
  a bug in itself; what would be a bug is it happening for a car that fits;
- a leftover `import = slot:city:model` in `JERICHO/CONFIG/carhacks.ini`, which claims a
  slot for another module and overrides the placement. `launch_cc_select.bat` neutralises
  it; neutralise it by hand before judging any placement result;
- a launch that never started a match (`JER_EVENT_CAR_DATA_SOURCE` only fires inside a
  level load), which shows as *no* `profile ... ->` lines rather than a failed one.

## The select flow

Three native frontend menus (`jer_frontend.h`), raised by `-ccmenu` or the main
menu's **Deathmatch** row (the renamed Undercover entry):

1. **`cc.arena`** — the four cities. Cross picks one.
2. **`cc.veh.<city>`** — `< CAR >`: one row, Left/Right cycles it, with the
   car's own frontend icon beside it. Cross stages the pick and opens…
3. **`cc.opponents`** — `< N OPPONENTS >`: one row, Left/Right cycles **0..6**
   (`CD2_AI_MAX`). Cross starts the match.

The match itself is a free-roam **TAKE A RIDE** — single player, on the chosen
city's **multiplayer-map arena 0** — with the chosen car and opponent count. The
opponent count is the match setting `ai_opponents`, so the pause menu can still
change it mid-match.

A headless run cannot drive the menus at all (the module screens ignore input
with no pad), so `CC_FORCE_ARENA` + `CC_FORCE_CAR` make the flow launch the match
from the frame handler instead of waiting for a confirm, and
`CC_FORCE_OPPONENTS` sets the count. A scripted run only — a pad still gets the
menus:

```sh
CC_FORCE_ARENA=3 CC_FORCE_CAR=6 CC_FORCE_OPPONENTS=6 \
  JERICHO_dev.exe -nointro -ccmenu -frames 900
```

## The test roster

Twelve profiles, each also carrying a special (see [`SPECIALS.md`](SPECIALS.md)):

| internal | display | home | vehicle | model | A/S/H/P | special | palette |
|---|---|---|---|---|---|---|---|
| `hornet` | Hornet | Chicago | **CROSSFIRE** | 2 | 2/4/4/3 | Spike Storm | 3 |
| `avalanche` | Avalanche | Vegas | Vegas | 11 | 4/2/3/4 | Monster Crush | – |
| `corvo` | Corvo | Rio | Rio | 0 | 3/3/3/4 | Siren's Wrath | – |
| `bruxa` | Bruxa | Rio | Rio | 2 | 4/2/3/4 | Double Boom | 2 |
| `highwayman` | Highwayman | Havana | Havana | 1 | 3/3/3/5 | Breath of Fire | 2 |
| `deadstar` | Deadstar | Rio | Rio | 12 | 4/3/2/4 | Death Dash | – |
| `obelisk` | Obelisk | Rio | Rio | 9 | 3/3/2/5 | Missile Barrage | – |
| `bootlegger` | Bootlegger | Vegas | Vegas | 1 | 2/5/4/3 | Lead Hail | 1 |
| `invocada` | Invocada | Rio | Rio | 3 | 4/3/2/5 | Cyclone | 0 |
| `fixer` | Fixer | Chicago | Chicago | 2 | 3/3/4/5 | Laser Lock | 3 |
| `wheelman` | Wheelman | Chicago | Chicago | 12 | 4/3/3/4 | Shockwave | – |
| `deathcruiser` | Death Cruiser | Vegas | **CROSSFIRE** | 1 | 5/2/2/2 | Death Cruiser (placeholder) | – |

The numbers are first-test placeholders — meant to be tuned. Obelisk's model is
9; the old C row's 11 was an accidental change. Death Cruiser uses the pack's
slot 1 body (currently the VEGAS_model1 donor) and its special only plays the
weapon-pickup cue for now.

## Reading a run

At boot the module dumps the whole roster (`cd2VehDumpProfiles`), and at level
start the resolution + the applied cosmetics:

```
[cainescrossfire] profiles: 12 registered
[cainescrossfire]   [0] hornet (Hornet) home CHICAGO vehicle CROSSFIRE/2 stats A2 S4 H4 P3 special special_hornet "Spike Storm" x3 rec600 palette 3
[cainescrossfire] profile hornet: bank CROSSFIRE registered as city 11 (..\JERICHO\MODS\cainescrossfire\vehicles\build\CROSSFIRE.LEV)
[cainescrossfire] profile hornet -> resident slot 10 (imported CROSSFIRE model 2)
[cainescrossfire] profile hornet: slot 10 cosmetics mass=3000 power=4400 traction=4096 wheelSize=53 topSpeedPct=115
```

A profile whose bank is not installed logs the unresolved source and is skipped;
the rest of the roster still loads.

## Known limits (first cut)

- The car's paint is written to `cp->ap.palette`; a model with no extra palette
  is left alone (palette `-1`). A pack's paints are its own, so a stock palette
  index may need retuning after a body moves to a bank.
- A profile's `topSpeedPct` is a sim-level nudge (it cannot be a `CAR_COSMETICS`
  field), applied in `cd2GetStats`.
- A bank referenced only by `vehicle.bank` must already be registered by
  carhacks/cities.ini; add `lev`/`lcf` to the profile to let the module register
  it on demand.
