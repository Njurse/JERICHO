# JerAI — general-purpose AI driver for the player's car

A COMPILED-IN (deep) JERICHO module that takes the player's car and drives it
with **A\* pathfinding over an engine occupancy grid**, a set of modular
behaviors (joyride/random-wander first), in-game debug visualization and a small
control menu.

It is compiled into the exe (no `runtime = "dll"` in `mod.toml`) because it
uses the engine's **compiled-in** road graph (`JerRoadInfoAt`) and 3D debug
lines (`Debug_AddLineDepth`), and reads/writes the car and player data globals
directly — the same reason the `mp` module (which the AI search code is lifted
from) is compiled in.

## Status

Under construction. The module entry, the lifted/namespaced AI search library,
the road-graph extractor, the driver core, the debug visualization and the
control menu land in phases; see the plan and the boot log.

## Layout

| File | Purpose |
|------|---------|
| `jerai.c/.h` | module entry point + tunables (config namespace `JerAI`) |
| `jerai_map.*` | the occupancy grid (engine probes: `CellEmpty`, `JerRoadAt`, `MapHeight`) |
| `jerai_star.*` | A\* over the grid |
| `jerai_local.*` | the local search ("get back to a road") |
| `jerai_road.*` | the engine road graph (surfaces + `ConnectIdx`) as nodes/edges |
| `jerai_drive.*` | pad injection + waypoint following |
| `jerai_behavior.*` | the modular behavior table |
| `jerai_viz.*` | debug drawing (grid / path / road graph / HUD) |
| `jerai_menu.*` | the in-game control menu |
| `jerai_dump.*` | offline road-graph export |

## Tunables (`JERICHO/CONFIG/JerAI.ini`, namespace `[JerAI]`, read live)

See `jerai.h` for the key names and defaults.

## Build

```
JERICHO\build_game.bat        (the game exe; the module is compiled in)
```

Enable it from Options → JERICHO (mods menu), or set `JerAI = 1` in
`JERICHO/CONFIG/modlist.ini`.
