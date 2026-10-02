# carhacks — vehicle availability + cross-city vehicle imports

carhacks is a standalone JERICHO module that **makes vehicles the game does not
normally offer selectable and drivable**.

Driver 2 ships thirteen car models per city (`CARMODEL_0..12`), but a level only
ever loads *its own city's* pool, and the frontend gates most of that pool behind
progression. carhacks opens both doors:

- **availability** — the frontend stops hiding the extra vehicles in the car list;
- **cross-city imports** — a level can load *another city's* vehicles and put them
  into its own resident model slots, so a Chicago car can be driven on a Havana
  level, a Rio truck on a Vegas level, and so on.

It used to live inside **Caine's Crossfire** and be
registered from that module's entry. It is its own compiled-in module now so it can
sit *next to any other module* — in particular **multiplayer** (`mp`) — without
dragging the whole total-conversion in. `carhacks.c/.h` stay host-agnostic (engine
globals via `exports.def` + the public JERICHO SDK only), so the unit can still be
compiled into another module unchanged; `carhacks_module.c` is the thin wrapper that
turns it into a module.

---

## Quick start

It is **off by default** (`mod.toml: default-enabled = false`). Turn it on in
`JERICHO/CONFIG/modlist.ini`:

```
carhacks = 1
```

It is a **compiled-in** module (no `runtime = "dll"`): MSVC cannot import the game's
data globals (`CarAvailability`, `carNumLookup`, `wantedCar`) into a DLL, and
carhacks reads and writes exactly those.

Then, in `JERICHO/CONFIG/carhacks.ini` (section `[carhacks]`), the out-of-the-box
hacks are already on. A config that imports three other cities' cars into a level
and puts them on the ground to look at:

```ini
[carhacks]
cross_city_vehicles = 1      ; turn the cross-city half on (opt-in)
two_guest_cities    = 1      ; measurement lever: allow more than one foreign city
import              = 4:0:8, 5:1:9, 6:3:12   ; slot:city:model, ...
spawn_imports       = 1      ; place one car per imported city ahead of the player
```

Every key is listed under [Config reference](#config-reference).

---

## Where to run things (and which copy is which)

**The repo tree is the source of truth.** Edit and run everything from
`JERICHO/MODS/<mod>/…`.

`src_rebuild/bin/Release_dev/` is **build output**. `.gitignore` excludes `*bin/`, and
every build refreshes it from the repo:

| what | where it ends up | why |
|---|---|---|
| the mod sources, docs and `mod.toml` | mirrored to `bin/<Config>/JERICHO/MODS/` | the game loads the compiled mods and reads `CONFIG/`, so a run is self-contained |
| **the tools** (`<mod>/tools/*`) | **repo only** — the build removes them from the mirror | they are source, not game data. Mirroring them put every tool in two places, and a `bin/` copy is a snapshot that goes stale |
| `JERICHO/CONFIG/*.ini` | seeded once, then updated in place | so a runtime toggle (which modules run, a mod's own settings) survives a rebuild |

The tools drive the game by resolving `$BIN` (the repo's
`src_rebuild/bin/Release_dev`), so they work wherever they are run from — but one that
finds itself under a `bin/` path prints a NOTE saying so, because that copy is a
snapshot and debugging it is debugging the wrong file.

## The shape of the module

| where | what |
|---|---|
| `carhacks.c/.h` | registration, the six event handlers, the config keys |
| `carimport.c` `carselect.c` `net.c` `spawn.c` | the import, the car-select
menu, the session wiring, the spawn lever |
| `docs/` | `CROSS_CITY.md`, `HACK.md`, `VEHICLES.md`, `PALETTES.md`, `VRAM.md`
— moved here from Caine's Crossfire, which keeps only `FORMATS.md` |
| `tools/` | the `chk_*.sh` harnesses **and** the car-data tools
(`vrammap.py`, `levgeom.py`, `levmodels.py`, `levpages.py`, `levpalette.py`,
`make_vram_issues_png.py`) |


Three independent hacks, each one row of the table in `carhacks.c` and each gated by
its own key, so they switch on and off separately:

| hack | key | default | what it does |
|---|---|---|---|
| Unlock extra vehicles | `unlock_extra_vehicles` | **1** | lift the progression + single-player gate on the frontend's extra vehicles |
| Cross-city vehicles | `cross_city_vehicles` | **0** (opt-in) | import vehicles that belong to *another* city's data |
| Car select menu | `car_select_menu` | **1** | replace the stock Take-a-Ride car screen with the menu that carries the city-roster row |

and six source files, one concern each:

| file | what lives there |
|---|---|
| `carhacks.c` / `.h` | the module entry, the hook table, the availability hack, and the shared logging rule |
| `carimport.c` / `.h` | the **cross-city import**: the config, the slot bookkeeping, the guest-city gate |
| `carselect.c` | the in-game car-select menu ([`CARSELECT.md`](CARSELECT.md)) |
| `spawn.c` | the `spawn_imports` measurement lever |
| `net.c` / `net.h` | the multiplayer channel and the import-set agreement ([`MP_ADAPTER.md`](MP_ADAPTER.md)) |
| `carid.h` | a car's identity — **(city, model)** — and its wire form |

---

## Problem 1 — the frontend hides the cars

Handled by `unlock_extra_vehicles` (`JER_EVENT_CAR_AVAILABILITY`, default **on**).

The frontend builds a ten-entry car list per level and maps it through
`carNumLookup`, the level's model-number table. Which entries you may actually pick
is decided by progression flags and by whether the session is single-player — the
engine effectively hides the tail of the list from you.

carhacks answers the engine's question as it builds that list: **unlock it**.

```
[JER_ARGS_CAR_AVAILABILITY]  level in, result out (1 = unlock the extra vehicles)
```

`car_list = 8,9,10` additionally *replaces* the last slots of the level's frontend
list with those model numbers, so you can put specific cars in front of the player
rather than merely un-hiding the stock tail.

---

## Problem 2 — a level only knows its own city

Handled by `cross_city_vehicles` (default **off**; opt-in because it is the part
that changes engine state).

This is the substantial half, and the rest of this section is what it does.

### Why it needs engine support at all

A level's car models are not loose files. They live **inside the level file**, in a
`CAR` lump, as compressed geometry plus texture-page data ([`FORMATS.md`](docs/FORMATS.md)).
There is no cross-city loader in the game, so "drive a Havana car on a Chicago
level" means *reading another level file's centre section and building models out of
it* — which is what the engine-side cross-city API (`InitCarImport`,
`GetCarImport*`, `ReadCarImportFileForCity` in `src_rebuild/Game/C/models.c`) exists
to do. carhacks is the policy on top: **which** city, **which** car, **which** slot.

### The import, step by step

The order matters, and every step has cost a bug. See
[`CROSS_CITY.md`](docs/CROSS_CITY.md) and
[`HACK.md`](docs/HACK.md) for the walkthrough and the traps.

**1. The engine asks.** As a level sets up its resident car models, it fires
`JER_EVENT_CAR_DATA_SOURCE` and hands carhacks the two arrays it is about to use:

```
[JER_ARGS_CAR_DATA_SOURCE]  level in
  models[]      out   residentCarModels[]  — the model number each slot wears
  modelSource[] out   -1 = the level's own city, else the city whose LEVEL
                      file supplies that slot's model, per slot
  sourceLevel   out   the city whose LEVELS folder to read (-1 = own)
```

**2. The module answers.** carhacks writes `import = slot:city:model` into those
arrays: `models[slot] = model`, `modelSource[slot] = city`. That is the whole
contract — everything after this is the engine following those two arrays.

> carhacks answers this event at **priority 10**, deliberately *after* **mp**'s
> handler at priority 0, and skips any slot mp already claimed. See
> [Slot ownership](#slot-ownership-and-why-it-is-a-contract).

**3. The engine reads the foreign level file.** For every city named in
`modelSource[]`, the engine opens that city's level file, finds the `CAR` lump and
loads its model list, its *page* lists (the `perm` and `spec` texture sets) and the
page base offset. One import block per city — `gCarImports[4]` — so a level can hold
several cities at once.

**4. Each slot's model is built from its own city.** The normal builder walks the
model's geometry; with an import, it asks for the model through
`GetCarImportModels(slot)` / the slot's city rather than the level's own table, so
slot 5 really does come out as a Havana car while slot 2 stays a Chicago one.

**5. The polys are re-pointed at a free page index.** A texture page is referenced
by **index**, and a set index holds one meaning at a time — the host level has
already claimed most of them. So as the imported car's polys are converted, each
one is translated onto a *different, free* index (`CarImportDstSet` /
`CarSetRemap`). Get this wrong and the car renders with the host city's textures
([`HACK.md`](docs/HACK.md) — "the part kept the host's
texture").

**6. The page and its CLUT are pinned in at draw time.** `LoadImportedTPages` records
a **pin** per imported page — which set, which destination index, the byte offset and
length of the page inside the foreign level file, the VRAM rectangle it would prefer
to reuse, and *which city it came from*. At draw time `CarImportPin` reads those
bytes (`ReadCarImportFileForCity(pin.city, …)` — never a level-wide singleton),
uploads them to a VRAM page slot, and uploads the page's CLUT rows. If no page slot
is free it evicts a world page rather than refuse.

The palette side runs alongside this: each city's car-palette rows are a separate
per-city problem in `cars.c`, and the **CLUT column is the binding constraint** —
see [Limits](#limits--what-it-does-not-do-today).

### Slot ownership, and why it is a contract

A *resident model slot* is the scarce resource, and every module that wants a car
shares the same `MAX_CAR_RESIDENT_MODELS` array:

| slots | who |
|---|---|
| `0..4` | the level's own city's civilians — `mission.c:350-354` fills these from the mission header, and `4` is the engine's own traffic filler (`dr2limits.h:23-28`) |
| `5..6` | **free** — the spare pair. This is where imports go, and where **mp** puts an extra player's car |
| `7` | the level's special body (`SPECIAL_CAR_SLOT`) |

Two of the civilian slots are load-bearing and must not be imported over casually:
**slot 3 is the cop car** (`externalCopModel = residentCarModels[3]`, `civ_ai.c:3390`)
and **slot 4 is the traffic filler**. A set *may* name 0..4 — the parser accepts any
slot below the count — but that recolours the level's own traffic, because traffic
rolls slots 0/1/2/4 (`civ_ai.c:47`). The module's own choosers start at **5**.

With `mp` loaded, slots **5 and 6** belong to the session (one per additional
player). This is why carhacks' `CAR_DATA_SOURCE` handler runs at priority 10: the
engine hands over `models[]` *after* mp has written its own, so carhacks can see
which slots are taken and skip them (`chkImportSlotFree` /
`chkImportSetEngineModels`). Two modules writing the same slot is a correctness bug,
not a cosmetic one — it is what [`MP_ADAPTER.md`](MP_ADAPTER.md) calls the
"slot ownership" contract.

### A car's identity is (city, model)

A model number on its own is **not** an identity. Every city ships `CARMODEL_0..12`
and the same number is a *different vehicle* in each city — model 9 is one car in
Chicago and another in Rio ([`VEHICLES.md`](docs/VEHICLES.md)).
So carhacks names a car as a pair, `carid.h`:

```
(city, model)      city 0..3 = CHICAGO/HAVANA/VEGAS/RIO
                   model 0..12
CHK_CITY_NATIVE    "the level's own city" — no import implied
CHK_MODEL_NONE     "no car"
```

Two bytes on the wire, and **versioned**: a peer that does not understand the
version refuses the set rather than guesses. This is the schema the whole module is
written against, and it is what a peer needs to describe a car it is driving.

---

## The car-select menu

Handled by `car_select_menu` (default **on**) — [`CARSELECT.md`](CARSELECT.md).

Rather than editing the stock screen, carhacks *replaces* the Take-a-Ride car screen
with a JERICHO menu that has an extra **city-roster** row: pick a city, browse that
city's list, and the pick is what the level then imports for you. It is the frontend
half of the same mechanism described above.

In a live mp session the stock screen stays mp's — the roster is the host's business.

---

## Multiplayer

Handled by `chkNet*` in `net.c` plus `mp_agree_imports` (default **on**) —
[`MP_ADAPTER.md`](MP_ADAPTER.md).

carhacks opens its **own channel over the JERICHO net bridge** (`jer_net`), so the
machines in a session can agree:

- **which import set** is in force — by default the **host's** wins for everyone
  (`mp_agree_imports = 1`); with it off, each machine keeps its own;
- **what each player is driving**, as a `(city, model)` identity;
- and, when this machine is asked to draw a *remote* player's car it does **not**
  hold, `JER_EVENT_CAR_PEER_DRAW` says so and supplies the palette to substitute —
  so a peer shows up as a stand-in rather than as nothing, or as the wrong car.

---

## Config reference

Section `[carhacks]` of `JERICHO/CONFIG/carhacks.ini`. Everything is read live, so a
change applies on the next level; nothing is cached.

### The three hacks

| key | default | meaning |
|---|---|---|
| `unlock_extra_vehicles` | `1` | unlock the extra vehicles on the frontend car list |
| `cross_city_vehicles` | `0` | turn the cross-city half on (everything below needs this) |
| `car_select_menu` | `1` | the car-select menu with the city-roster row |

### Cross-city — read only when `cross_city_vehicles = 1`

| key | default | meaning |
|---|---|---|
| `import` | — | `slot:city:model, ...` — the import set. `import = 4:0:8, 5:1:9` puts `city 0 CHICAGO model 8` into slot 4 and `city 1 HAVANA model 9` into slot 5 |
| `source_city` | `-1` | make the whole level read its car data from this city instead (`0..3`); `-1` = the level's own |
| `car_list` | — | `8,9,10` — replace the last slots of the level's frontend car list with these model numbers |
| `traffic_model`, `traffic_slot` | — | put a foreign model into a civilian slot so ambient traffic uses it |
| `mp_agree_imports` | `1` | in a session, let the host's import set win for everyone |
| `spawn_imports` | `0` | **measurement lever**: place one car per imported city ahead of the player, once per level |
| `spawn_spacing` | `1500` | the gap between those placed cars, in world units — the default suits a long body (bus, fire truck, semi); echoed on the spawn summary line so a run records the value |
| `two_guest_cities` | `0` | **measurement lever**: let more than one foreign city into the set. Off by default because the CLUT column overflows with one; see [Limits](#limits--what-it-does-not-do-today) |

Cities are `0..3` = CHICAGO, HAVANA, VEGAS, RIO. Models are `0..12`.

---

## Hooks

Every hook is registered from `carhacks_register` and returns `JER_RESULT_CONTINUE`,
so carhacks never swallows an event another module needs.

| event | prio | what carhacks does with it |
|---|---|---|
| `JER_EVENT_CAR_AVAILABILITY` | 0 | unlocks the extra vehicles on the frontend car list |
| `JER_EVENT_CAR_DATA_SOURCE` | **10** | writes the import set into the engine's `models[]` / `modelSource[]` — *after* mp, so it can skip mp's slots |
| `JER_EVENT_CAR_PEER_DRAW` | 0 | answers how a remote player's car is drawn here, and the palette to substitute if we do not hold it |
| `JER_EVENT_FRONTEND_ENTERED` | 0 | arms the car-select menu |
| `JER_EVENT_LEVEL_LAUNCH` | 0 | deliberately a **no-op**, and kept so nobody "fixes" it back: it fires *before* the level's car data exists, so publishing the import set here sent an empty one. The real moment is when the set finishes building |
| `JER_EVENT_NET_RECV` | 0 | the mp channel |
| `JER_EVENT_FRAME` | 0 | three of them — the menu's input, the net keepalive, and the spawn lever. Each is cheap when idle |

---

## Logging

One rule per call, so no future line has to guess (also stated at the top of
`carhacks.h`):

| call | for | notes |
|---|---|---|
| `printInfo(...)` | every **diagnostic** line | the engine's own informational channel — the same one emitting the `cross-city:` / `JERICHO-*` lines carhacks' messages sit beside, and the one the harnesses read out of a run's captured stdout. Prefix `[carhacks]` / `[carhacks/net]`. |
| `jer_error(...)` | a line the **player** must see | a dropped car, a refused pick, a menu that will not come up. Logs the message verbatim as `[error] …` **and** raises the on-screen notice — so it is the *single* call for a player-facing refusal. Do not pair it with a `printInfo`. |
| `ctx->jer_log(ctx, …)` | a **module entry / registration** function | only where `ctx` is the handle in hand (`carhacks_register`, `chkNetRegister`, `chkCarSelectRegister`); it lands in the same log as `printInfo`. |

---

## Limits — what it does not do (today)

Stated plainly, because each one has a doc and a plan behind it.

- **The CLUT column overflows with one import.** A level's car palettes and the
  imported ones contend for the same VRAM CLUT rows, and the import's palette table
  is filtered down but still lands in the level font's band. The colours of imported
  cars are therefore **not right** in a multi-city run — geometry and placement are,
  which is what the test tools assert. This is the next unit.
- **One foreign city per level**, until that budget is fixed
  (`two_guest_cities` is the lever that overrides the gate for measurement).
- **A foreign car has to be driven, or placed.** Filling a resident model slot does
  not create a vehicle, and the engine spawns only slots `0/1/2/4` as traffic — so
  slots 5/6 can never appear on their own. That is exactly what `spawn_imports`
  exists for.
- **A peer driving a car this machine does not hold** is drawn from this machine's
own slot with a **neutral palette** — a stand-in, never the owner's car — until
the hot-load hand-off lands ([`MP_ADAPTER.md`](MP_ADAPTER.md)).

---

## Testing it

The cross-city test suite and the launchers live in this module's `tools/`
(`chk_suite.sh`, `launch_mp_*.bat`); they write `JERICHO/CONFIG/carhacks.ini` and
enable this module by id. `chk_suite.sh` is the regression gate: a stock control, the
imported PLAYER once per host city, a 3-city `city mix`, and a foreign **traffic** model.

This module's own tools (in `tools/`):

| tool | what for |
|---|---|
| `chk_single_city_playtest.sh [city] [frames]` | one host level, one car from every **other** city, **placed on the ground** to look at. `CHK_SHOW=1` streams the game log live. The watchable one |
| `chk_all_cities.sh [frames]` | the same, but for **every** host level in turn |
| `chk_mashup.sh [host] [mix] [frames]` | the biggest mix the engine can hold: cars from **2-4 cities** shuffled into the resident slots that are placed *and* driven, verified `built N/N, spawned N/N` |

**No frames argument means MANUAL**: `-frames` is left off entirely, so the game runs
until you close it and the tool moves on then — one close per level in the suite, so you
go to the next test when you are ready rather than when a timer says so. Pass a number
for a timed run; `manual` and `0` spell the default out, and the banner prints which one
it resolved.

All of them show the imported cars with the **wrong colours** until the CLUT band
placement lands (see [Limits](#limits--what-it-does-not-do-today)) — they judge
geometry, placement and mix, not paint.
| `chk_mp_foreign.sh` | a **real mp pair** with a different foreign city on each side, reporting what each machine loaded and what each player ended up driving |

Both city tools assert the same three things per level, and **fail** if any is
missing — loaded, built *and* placed:

```
-> lumps 3/3, geometry 3/3, spawned 3/3
```

The third matters: "loaded and built" is not "visible". See below.

`chk_mp_foreign.sh` drives mp's harness (`JERICHO/MODS/mp/tools/mp_pair.bat`), so
both `mp` and `carhacks` must be enabled for it.

---

## The `spawn_imports` lever — seeing the cars

An import fills a resident **model slot**; it does not create a vehicle. Nothing in
the engine ever makes one from slots 5/6, so a foreign car is invisible unless you
drive it:

- traffic picks its model from `modelRandomList` (`civ_ai.c`), which names slots
  `0/1/2/4` only;
- `residentCarModels[5]/[6]` are written and read by nobody;
- carhacks itself did not spawn.

A level really can load three cities' car data, log `geometry from HAVANA model 9`
for each, and show none of them. With `spawn_imports = 1`, `spawn.c` installs a
FRAME hook that fires **once per level**, as soon as the level is live *and* the
player is in a car, and puts one car per imported city **in a line ahead of the
player**:

```
[carhacks] spawn: CHICAGO model 8 (resident slot 4) in CAR_DATA slot 2, palette 0,  900 ahead - player (24453,30,-497793) car (25353,30,-497794)
[carhacks] spawn: HAVANA  model 9 (resident slot 5) in CAR_DATA slot 3, palette 0, 1800 ahead - player (24453,30,-497793) car (26253,30,-497794)
[carhacks] spawn: 3 imported car(s) placed ahead of the player, 1500 units apart
```

They go **forward**, along the matrix's third column — the engine's own "a point
ahead of a car" (`cop_ai.c` reads the same pair for 400 units ahead; `handling.c`
derives `hd.direction` from it). The side is the kerb, or a wall, at any spawn point.
Both positions are logged so the line is checkable by hand.

It is a **measurement lever, not a feature**: the cars are `CONTROL_TYPE_CUTSCENE`
(nothing drives them, so they stay where they are put) and the colours are not right
until the CLUT work lands. Full detail:
[`CROSS_CITY.md`](docs/CROSS_CITY.md), "Seeing the imported
cars".

---

## Debugging a run

Everything is in the run's log; grep for these:

| line | tells you |
|---|---|
| `cross-city: car data from <CITY>` | that city's `CAR` lump loaded, with its byte cost |
| `cross-city: <CITY> page lists - N permanent sets, M special sets, 6/6 car sets present` | the page lists parsed, per city |
| `cross-city: slot N geometry from <CITY> model M` | slot N is really built from that city's model |
| `cross-city: <CITY> set S -> index D, ... bytes at +O, ... clut rows` | the page's index translation and its cost |
| `cross-city: <CITY> set S is not in its page list - skipped` | the set was not found — usually a wrong `import` entry |
| `[carhacks] spawn: ...` | the spawn lever placed a car |
| `JERICHO-CLUT:` | the level's own pages need N..M CLUT rows, and what the slot band reserves |
| `JERICHO-VRAM:` | the texture/CLUT budget, and whether the CLUT strip overflowed |
| `JERICHO-HEAP:` | level heap and car-poly usage |
| `[error] …` | a player-facing refusal — the thing the player was told |

---

## Docs

The mechanism and the city data formats live in this module's `docs/`, beside the
code they describe. Note that engine `file:line` citations throughout
these docs are **approximate by design** - the engine moves, the mechanism does
not - so confirm a citation before relying on the exact line:

- [`CROSS_CITY.md`](docs/CROSS_CITY.md) — what a cross-city import is, cost and lifetime, the budget, the traps
- [`HACK.md`](docs/HACK.md) — the mechanism in the order it runs
- [`VEHICLES.md`](docs/VEHICLES.md) — model numbers → what they are, per city
- [`FORMATS.md`](docs/FORMATS.md) — the level-file / lump formats
- [`PALETTES.md`](docs/PALETTES.md) — car palettes and CLUT rows
- [`VRAM.md`](docs/VRAM.md) — the VRAM layout and how to measure it

This module's own docs:

- [`CARSELECT.md`](CARSELECT.md) — the car-select menu
- [`MP_ADAPTER.md`](MP_ADAPTER.md) — the identity schema, the net channel, drawing a peer's car, and the slot-ownership contract
