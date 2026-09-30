# carhacks — vehicle-availability + cross-city vehicle hacks

A standalone JERICHO module that makes vehicles the game does not normally offer
selectable or usable.

It used to live inside **Caine's Crossfire** (`cainescrossfire/carhacks/`) and be
registered from that module's entry; it is now its own compiled-in module so it
can be enabled *next to any other module* — in particular the **multiplayer**
(`mp`) module — without dragging the whole total-conversion in. `carhacks.c/.h`
are kept host-agnostic (engine globals exported via `exports.def` + the public
JERICHO SDK only), so the unit can still be compiled into another module
unchanged; `carhacks_module.c` is the thin wrapper that turns it into a module.

## Enabling it

It is **off by default** (`mod.toml: default-enabled = false`). Turn it on in
`JERICHO/CONFIG/modlist.ini`:

```
carhacks = 1
```

It is a **compiled-in** module (no `runtime = "dll"`): MSVC cannot import the
game's data globals (`CarAvailability`, `carNumLookup`, `wantedCar`) into a DLL,
and carhacks reads and writes exactly those.

## The hacks

Each hack is one row of the table in `carhacks.c` and is gated by its own config
key in `JERICHO/CONFIG/carhacks.ini` (section `carhacks`), so they switch on and
off independently.

| key | default | what it does |
|---|---|---|
| `unlock_extra_vehicles` | 1 | lift the progression + single-player gate on the frontend car list's extra vehicles (`JER_EVENT_CAR_AVAILABILITY`) |
| `cross_city_vehicles` | 0 | import vehicles that belong to *another* city's data (`JER_EVENT_CAR_DATA_SOURCE`) |
| `car_select_menu` | 1 | replace the stock Take-a-Ride car screen with the menu that has the city-roster row ([`CARSELECT.md`](CARSELECT.md)) |
| `mp_agree_imports` | 1 | in a session, let the HOST's import set win for everyone ([`MP_ADAPTER.md`](MP_ADAPTER.md)); off ⇒ each machine keeps its own |

Cross-city keys (only read when `cross_city_vehicles = 1`):

| key | meaning |
|---|---|
| `car_list` | `8,9,10` — replaces the last slots of the level's frontend car list with those model numbers |
| `source_city` | the city a level reads its car data from (`0..3`) |
| `import` | `slot:city:model, ...` — write foreign models into resident slots |
| `traffic_model`, `traffic_slot` | put a foreign model into a civilian slot so ambient traffic uses it |
| `mp_agree_imports` | in a session, let the host's import set win for everyone ([`MP_ADAPTER.md`](MP_ADAPTER.md)); off ⇒ each machine keeps its own |
| `spawn_imports` | `1` ⇒ put one car of each imported city in a line ahead of the player, once per level, so the geometry can be looked at (see below) |

### `spawn_imports` — seeing the cars

An import fills a resident **model slot**; it does not create a vehicle, and nothing
in the engine spawns slots 5/6 (traffic rolls 0/1/2/4 only). So a foreign car is
invisible unless you drive it. `spawn_imports = 1` in `carhacks.ini` puts one car of
each imported city in a line ahead of the player, once per level, so the geometry
can be looked at. Off by default; it is a measurement lever, and the colours are not
right until the CLUT band placement lands.

Full detail, and why: [`CROSS_CITY.md`](CROSS_CITY.md), "Seeing the imported cars".

## Hooks

| event | what it does |
|---|---|
| `JER_EVENT_CAR_AVAILABILITY` | unlocks the extra vehicles on the frontend car list |
| `JER_EVENT_CAR_DATA_SOURCE` | writes the level's import set into the engine's `models[]` / `modelSource[]` |
| `JER_EVENT_CAR_PEER_DRAW` | **answer how a remote player's car is drawn here** — whether this machine really holds that player's vehicle, and the palette to use if it does not ([`MP_ADAPTER.md`](MP_ADAPTER.md)) |

## Logging

One rule per call, so no future line has to guess (also stated at the top of
`carhacks.h`):

| call | for | notes |
|---|---|---|
| `printInfo(...)` | every **diagnostic** line | the engine's own informational channel — the same one that emits the `cross-city:`/`JERICHO-*` lines carhacks' messages sit beside, and the one the harnesses read from a run's captured stdout. Prefix with `[carhacks]` / `[carhacks/net]`. |
| `jer_error(...)` | a line the **player** must see | a dropped car, a refused pick, a menu that will not come up. Logs the message verbatim as `[error] …` **and** raises the on-screen notice, so it is the *single* call for a player-facing refusal — do not pair it with a `printInfo`. |
| `ctx->jer_log(ctx, …)` | a **module entry / registration** function | only where `ctx` is the handle in hand (`carhacks_register`, `chkNetRegister`, `chkCarSelectRegister`); it lands in the same log as `printInfo`. |

## Docs

The mechanism and the city data formats are documented next to the engine code
they reverse-engineer, in Caine's Crossfire's folder (so they were not moved with
the code):

- [`cainescrossfire/carhacks/CROSS_CITY.md`](../cainescrossfire/carhacks/CROSS_CITY.md) — what a cross-city import is, cost and lifetime
- [`cainescrossfire/carhacks/HACK.md`](../cainescrossfire/carhacks/HACK.md) — the mechanism in the order it runs, and every trap
- [`cainescrossfire/carhacks/VEHICLES.md`](../cainescrossfire/carhacks/VEHICLES.md) — model numbers → what they are, per city
- [`cainescrossfire/carhacks/FORMATS.md`](../cainescrossfire/carhacks/FORMATS.md), [`PALETTES.md`](../cainescrossfire/carhacks/PALETTES.md), [`VRAM.md`](../cainescrossfire/carhacks/VRAM.md)

This module's own docs:

- [`CARSELECT.md`](CARSELECT.md) — the in-game car select: a city-roster row in a JERICHO menu override
- [`MP_ADAPTER.md`](MP_ADAPTER.md) — the car-identity / import-set schema, the connection channel over the JERICHO net bridge (`jer_net`), how a peer's car is drawn (and corrected), and the measured one-foreign-city-per-level limit

## Test tooling

The cross-city test suite and launchers live in Caine's Crossfire's `tools/`
(`devcheck.sh`, `launch_mp_*.bat`) and write `JERICHO/CONFIG/carhacks.ini`; they
enable this module by id.

This module's own tool:

| tool | what for |
|---|---|
| `tools/chk_single_city_playtest.sh` | one host level, one car from every **other** city, **placed on the ground** to look at (`[city] [frames]`, `CHK_SHOW=1` to watch it live) — the watchable one |
| `tools/chk_mp_foreign.sh` | stress a **real mp pair** with a different foreign city on each side (three cities' car data at once) and report what each machine loaded and what each player ended up driving — see [`MP_ADAPTER.md`](MP_ADAPTER.md) |
| `tools/chk_all_cities.sh` | the same as the playtest but for **every** host level in turn, asserting `lumps 3/3, geometry 3/3, spawned 3/3` on each |

The all-cities tool turns on `spawn_imports` (below), so it also proves the cars are
actually put on the ground where they can be looked at.

It drives mp's harness (`JERICHO/MODS/mp/tools/mp_pair.bat`), so `mp` and
`carhacks` must both be enabled in `JERICHO/CONFIG/modlist.ini` for it.
