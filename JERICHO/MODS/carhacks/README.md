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

Cross-city keys (only read when `cross_city_vehicles = 1`):

| key | meaning |
|---|---|
| `car_list` | `8,9,10` — replaces the last slots of the level's frontend car list with those model numbers |
| `source_city` | the city a level reads its car data from (`0..3`) |
| `import` | `slot:city:model, ...` — write foreign models into resident slots |
| `traffic_model`, `traffic_slot` | put a foreign model into a civilian slot so ambient traffic uses it |

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
- [`MP_ADAPTER.md`](MP_ADAPTER.md) — the car-identity / import-set schema and the connection channel over the JERICHO net bridge (`jer_net`), for the `mp` module

## Test tooling

The cross-city test suite and launchers live in Caine's Crossfire's `tools/`
(`devcheck.sh`, `launch_mp_*.bat`) and write `JERICHO/CONFIG/carhacks.ini`; they
enable this module by id.
