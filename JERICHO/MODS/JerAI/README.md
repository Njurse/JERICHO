# JerAI — a general-purpose AI driver for the player's car

A COMPILED-IN (deep) JERICHO module that takes the player's car and drives it
with **A\* pathfinding over an engine occupancy grid**, a modular set of
behaviors (joyride/random-wander first), **in-game debug visualization** and a
small **control menu**.

It is compiled into the exe (no `runtime = "dll"` in `mod.toml`) because it uses
the engine's **compiled-in** road graph (`JerRoadInfoAt`) and the depth-sorted
world line (`Debug_AddLineDepth`), and reads/writes the car and player data
globals directly — the same reason the `mp` module (which the AI search code is
lifted from) is compiled in.

## How it drives

The car stays **the player's own car** (`CONTROL_TYPE_PLAYER`). JerAI produces a
`CAR_PAD_*` bitmask each frame and hands it to the engine through
`JER_EVENT_CAR_PAD`, so the engine's normal input path turns it into pedals and
steering — the same approach `mp`'s bot uses.

Two engine details that cost real time and are load-bearing:

- the injected pad must set `useAnalogue = 0`; steering from `CAR_PAD_LEFT/RIGHT`
  is only read when the analogue flag is clear (`handling.c`), so leaving the
  physical pad's flag set gives a car that accelerates but never turns;
- **`CAR_PAD_LEFT` DECREASES the heading.** `diff = want - dir`, so a positive
  error needs `CAR_PAD_RIGHT`. Getting this backwards makes the car steer away
  from its target, wind the error up to +/-180 deg and U-turn - which reads as
  "it keeps turning the wrong way";
- `CAR_PAD_HANDBRAKE` must never be emitted as a **pad bit**: the stock handler
  cycles `g_PlayerControlMode` on a handbrake press edge and would hand the car
  back to the engine AI. The wall-evasion handbrake below is applied through the
  `JER_EVENT_CAR_PAD` `handled` path instead (writing `cp->handbrake` with
  `handled = 1` skips the stock block, so the mode is never touched).

Driving is governed by:

- **pure-pursuit** path tracking: the car is projected onto the plan polyline and
  aims a distance further ALONG it, so it tracks the line instead of cutting to a
  far corner (and can never point at a waypoint it has already passed). The
  look-ahead **stretches with speed** (`JERAI_AIM_SPEED_GAIN`): a fast car looks
  further up the plan rather than chasing every wiggle and sawing at the wheel;
- a **directional guide**: the aim walk takes the first point that is both far
  enough ahead AND not behind the car (cos > -0.35 ~= within 110 deg, so a
  perpendicular intersection turn still counts), and the joyride goal picker skips
  destinations behind the car. Both exist to stop the car targeting something it
  has passed and turning back for it;
- **feelers** - short, FRESH, thin `CellEmpty` probes fanned in front of the car,
  which bias the aim around scenery the coarse (512-unit) and up-to-a-replan-old
  A\* grid cannot see. A wall on one side pushes the aim to the other; a street
  with walls both sides cancels out. The centre feeler brakes when the nose is
  genuinely blocked;
- a steering dead-band **with hysteresis** (start at 70, keep turning to 30) so
  it does not flip sides on noise, and a lift-off when a car is close ahead (no
  braking for traffic - that made the car crawl past oncoming vehicles);
- a **stuck recovery** that backs out of a wedge (and asks the behavior for a new
  destination) after ~1.3 s at a standstill, plus an **early reverse** the moment
  scenery is right on the nose while barely moving (backing off sooner grinds
  less than waiting out the timer).

## Car dynamics (speed, grip, corners)

Walking a line is not driving: a car carries momentum and its velocity can point
somewhere other than its nose. JerAI reads that (`JERAI_CARSTATE.slip`, the angle
between `st.n.linearVelocity` and the heading) and drives to a **grip budget**:

- `JerAiScanCurve` finds the sharpest turn on the plan within `corner_lookahead`
  (its heading change, how far off it is, and the arc it happens over);
- `JerAiSpeedTarget` turns that into a speed the car may carry now: the grip limit
  for the corner radius (`sqrt(corner_g * r)`), backed off by the braking distance
  to it (`sqrt(vCorner^2 + 2*brake_gain*dist)`) so the car lifts BEFORE the
  corner instead of at it. `speed_scale` is an overall aggression dial.
- the pad then brakes over that target, lifts slightly over, and powers out under;
- a **wall-evasion handbrake** tight-turns when scenery is close ahead;
- `grip` (100 = hooked up) falls with the slide, and is on the HUD.

The A\* cost model hugs the middle hard (a squared clearance penalty), so it
routes around a block rather than squeezing a short gap past a building.

## Behaviors (`mode`)

| id | name | what it does |
|----|------|--------------|
| 0  | Manual  | no pad injection — the car is the player's |
| 1  | Joyride | picks a random road sample nearby and drives to it, then repeats |

Adding a behavior is one function (`CAR_DATA* -> pad`) plus one row in
`jerai_behavior.c`'s table; the menu and config pick it up automatically.

## Visualization (`viz_*`)

Drawn in `JER_EVENT_DRAW_WORLD` with `Debug_AddLineDepth` (buildings occlude the
lines):

- `viz_road` — the engine road graph: nodes as crosses (yellow straight / orange
  curve / green junction) and the `ConnectIdx` edges as blue lines;
- `viz_grid` — the occupancy grid: blocked samples (walls) in red, road samples
  in green, within ~6,000 units of the car;
- `viz_path` — the current A\* plan (red), the goal cross (yellow) and the aim
  line (green);
- `viz_hud` — a one-line text readout (mode, speed, waypoints, graph size, lines).

## Immunity

While JerAI is driving (and `immune = 1`, the default) the player's car is
invulnerable: `JER_EVENT_GET_BUDDHA` clamps its damage below the totaled
threshold, and the frame hook keeps the wanted level at zero
(`felonyRating`/`copsAreInPursuit`/`CopsCanSeePlayer`/`pedestrianFelony`), so no
cop sirens chase the test drive. Manual play (`mode = 0`) is left untouched.

## Control menu

**L2** opens a small `jer_menu` list over the game: the behaviors, the four
visualization layers, *Dump road graph now*, and *Close*. It reads the PHYSICAL
pad (in the frame hook) while the car gets the synthetic one, so it works while
JerAI is driving. Every change is written to the config immediately.

## Road-graph dump

Set `dump_now = 1` (or pick *Dump road graph now*) and the module writes, into
the working directory:

- `JerAI_roadgraph.json` — nodes (surfId, kind, world x/y/z, lanes, speed limit)
  and edges (node index pairs);
- `JerAI_roadgraph.obj` — the same graph as geometry (world/4096, X mirrored).

## Layout

| File | Purpose |
|------|---------|
| `jerai.c/.h` | module entry, config, the CAR_PAD injection and the HUD |
| `jerai_internal.h` | shared state + file map (glue between this module's files) |
| `jerai_map.*` | the occupancy grid (engine probes: `CellEmpty`, `JerRoadAt`, `MapHeight`) |
| `jerai_mapgrid.c` | the pure grid queries + cost model |
| `jerai_star.*` | A\* over the grid |
| `jerai_local.*` | the local search ("get back to a road") |
| `jerai_road.*` | the engine road graph (surfaces + `ConnectIdx`) as nodes/edges |
| `jerai_dump.c` | the offline road-graph export |
| `jerai_drive.*` | pad injection + waypoint following |
| `jerai_behavior.*` | the modular behavior table |
| `jerai_viz.c` | debug drawing |
| `jerai_menu.c` | the control menu |

The search library (`jerai_map` / `jerai_mapgrid` / `jerai_star` / `jerai_local`)
is a namespaced copy of `mp`'s `ai/` — both modules are compiled into the same
exe, so the symbols must not collide. The pure half is exercised engine-free by
`src_rebuild/Game/C/JERICHO/test/test_jerai_path.c`.

## Tunables (`JERICHO/CONFIG/JerAI.ini`, namespace `[JerAI]`, read live)

See `jerai.h` for the key names and defaults. The car-dynamics knobs:

| key | default | meaning |
|-----|---------|---------|
| `corner_g` | 2 | lateral-g budget; bigger = more speed through the same curve |
| `brake_gain` | 1 | braking authority for the braking-distance term |
| `corner_lookahead` | 6000 | how far ahead the plan is scanned for corners (world units) |
| `speed_scale` | 100 | overall aggression: the corner target as a percentage |

`brake_gain` is in speed^2 per world unit; 40 swamps the term (the target never
drops), so keep it near 1.

### Not built, on purpose

An **apex / racing-line bias** (offsetting the aim toward the inside of a corner
to use the full road width) was proposed and deliberately NOT built. The A* cost
model already hugs the middle of the road hard, and this car's recurring failure
is grinding along walls, not leaving time on the table - pulling the aim off the
centreline toward an apex is exactly the change that would put it back into the
scenery. The feelers (which push the aim AWAY from a wall) are the safer
line-shaping lever and are what is used.

A grip-budget **steer-authority cap** was also dropped: the injected steering is a
digital pad, so authority cannot be scaled smoothly, and the analog channel is
neutered by `handling.c`'s return-to-centre block (it zeroes any analog angle
below ~65). Steering is damped with hysteresis plus the speed-scaled look-ahead
instead.

## Build

```
JERICHO\build_game.bat        (the game exe; the module is compiled in)
```

Enable it from Options → JERICHO (mods menu), or set `JerAI = 1` in
`JERICHO/CONFIG/modlist.ini`.
