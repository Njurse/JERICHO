# Vehicle profiles

The vehicle profile system is the roster of the game: one **profile** per
contestant's car, holding everything that makes that car *that car* — its
identity, the exact vehicle it maps to in a city's asset files, its 1..5 design
stats, its unique special weapon, and the physics it overrides.

It lives in `profiles/`:

| File | Role |
|---|---|
| `profile.h` | the schema: `CD2_VEH_PROFILE`, its sub-structs, and the registry API |
| `registry.c` | **the manifest** — one line per profile (`gVehRows[]`, index == `CD2_VEH_*`) |
| `rows/*.c` | one file per profile (the actual data) |
| `profiles_map.c` | resolves profiles into the engine (resident slots, cosmetics, paint) |

## Adding a profile

1. Copy a row file in `profiles/rows/`, change the values (keep the id order in
   `profile.h`'s `CD2_VEH_*` enum).
2. Add its `extern` to `profiles/rows/rows.h`.
3. Add it to `gVehRows[]` in `profiles/registry.c`.

The build globs `MODS/<mod>/**/*.c`, so a new row file compiles with no premake
edit. That is the whole loop — the rows are data, kept in their own files on
purpose.

## The row

```c
extern const CD2_VEH_PROFILE cd2VehRowHornet =
{
	CD2_VEH_HORNET, "hornet", "Hornet",       /* id, internalName, displayName */
	CD2_VEH_CITY_CHICAGO, 3,                  /* originCity, modelSlot          */
	2, 4, 4, 3,                               /* armor, speed, handling, special */
	CD2_WID_SPECIAL_HORNET,                   /* the special — by id only */
	{ 3000, 4400, 0, 0, 0, 0, 0, 0, 0, 115 }, /* phys overrides (0 = inherit)   */
	3                                         /* palette                        */
};
```

Three parts:

- **Identity + mapping.** `internalName` is code-facing (`hornet`); `displayName`
  is on screen (`Hornet`). A vehicle is identified by **(city, model number)** —
  `originCity` is a `LevelNames[]` index (Chicago 0, Havana 1, Vegas 2, Rio 3) and
  `modelSlot` is the number in that city's `CARMODEL_<n>` set (see
  `carhacks/VEHICLES.md`). The same model number is a different car in each city,
  which is why both are tracked.
- **Core stats.** Armor / Speed / Handling / Special Power, each 1..5. These are
  the design + HUD numbers; Speed and Handling also shape the sim (below).
- **Physics overrides.** Per-field substitutes for the vehicle's `CAR_COSMETICS`
  (`mass`, `powerRatio`, `traction`, `susCoeff`, `wheelSize`, `twistRateX/Y/Z`,
  `cogY`, plus a sim-level `topSpeedPct`). **`CD2_VEH_INHERIT` (0) means "keep the
  model's own value"** — so a profile can be heavy on one axis and still pull the
  rest from the original model. The override is written into
  `car_cosmetics[slot]` at level start, before any car is built.

## Where a profile meets the engine

Timing (verified in `main.c`): `SetupResidentModels` fires
`JER_EVENT_CAR_DATA_SOURCE`, then `LoadGameLevel → LoadCosmetics` fills
`car_cosmetics[]`, **then** `JER_EVENT_GAME_START`, and only after that are the
cars created. So:

1. **CAR_DATA_SOURCE** — each *fielded* profile's `(city, model)` is placed into
   a resident car slot:
   - a **native** profile (its city is the level's) reuses a resident slot that
     already holds its model; failing that it takes an empty **spare** slot, the
     level's own lump supplying the geometry;
   - a **foreign** profile is placed only when its city is the level's one
     **guest city**, into an empty spare slot (the engine imports from only one
     foreign city per level — `InitCarImport` holds a single city, `models.c`).
   Civilian slots (0..4) are never repurposed — they carry the level's own models
   and the ambient traffic, and stealing one is what made the level's cars look
   wrong. So a car belongs to the city it is picked in.
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
  when empty — the profiles whose home city is the level's own;
- plus the profile the **player** picked in the CC select flow;
- `CC_PROFILES` overrides the whole set for a headless run (never persisted).

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
  REDRIVER2_dev.exe -nointro -ccmenu -frames 900
```

## The test roster

Ten profiles, each also carrying a special (see [`SPECIALS.md`](SPECIALS.md)):

| internal | display | city | model | A/S/H/P | special | palette |
|---|---|---|---|---|---|---|
| `hornet` | Hornet | Chicago | 3 | 2/4/4/3 | Spike Storm | 3 |
| `avalanche` | Avalanche | Vegas | 11 | 4/2/3/4 | Monster Crush | – |
| `corvo` | Corvo | Rio | 0 | 3/3/3/4 | Siren's Wrath | – |
| `bruxa` | Bruxa | Rio | 2 | 4/2/3/4 | Double Boom | 2 |
| `highwayman` | Highwayman | Havana | 1 | 3/3/3/5 | Breath of Fire | 2 |
| `deadstar` | Deadstar | Rio | 12 | 3/5/3/4 | Death Dash | – |
| `obelisk` | Obelisk | Rio | 9 | 3/3/2/5 | Missile Barrage | – |
| `bootlegger` | Bootlegger | Vegas | 1 | 2/5/4/3 | Lead Hail | 1 |
| `invocada` | Invocada | Rio | 3 | 4/3/2/5 | Cyclone | 0 |
| `fixer` | Fixer | Chicago | 2 | 3/3/4/5 | Laser Lock | 3 |

The numbers are first-test placeholders — meant to be tuned.

## Reading a run

At boot the module dumps the whole roster (`cd2VehDumpProfiles`), and at level
start the resolution + the applied cosmetics:

```
[cainescrossfire] profiles: 6 registered
[cainescrossfire]   [0] hornet (Hornet) CHICAGO/3 stats A2 S4 H4 P3 special special_hornet "Spike Storm" x3 rec600 palette 3
[cainescrossfire] profile hornet -> resident slot 3 (level's own CHICAGO model 3)
[cainescrossfire] profile hornet: slot 3 cosmetics mass=3000 power=4400 traction=4096 wheelSize=53 topSpeedPct=115
```

## Known limits (first cut)

- The car's paint is written to `cp->ap.palette`; a model with no extra palette
  is left alone (palette `-1`).
- A profile's `topSpeedPct` is a sim-level nudge (it cannot be a `CAR_COSMETICS`
  field), applied in `cd2GetStats`.
