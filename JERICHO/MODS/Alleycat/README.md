# Alleycat — a general-purpose AI driver for the player's car

A COMPILED-IN (deep) JERICHO module that takes the player's car and drives it
with **A\* pathfinding over an engine occupancy grid**, a modular set of
behaviors (joyride, pursuit, police-evade), **in-game debug visualization**, a short
**HUD readout** in the top-left corner and a **pause-menu settings page** (L2).

It is compiled into the exe (no `runtime = "dll"` in `mod.toml`) because it uses
the engine's **compiled-in** road graph (`JerRoadInfoAt`) and the depth-sorted
world line (`Debug_AddLineDepth`), and reads/writes the car and player data
globals directly — the same reason the `mp` module (which the AI search code is
lifted from) is compiled in.

## How it drives

The car stays **the player's own car** (`CONTROL_TYPE_PLAYER`). Alleycat produces a
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
  look-ahead **stretches with speed** (`ALLEYCAT_AIM_SPEED_GAIN`): a fast car looks
  further up the plan rather than chasing every wiggle and sawing at the wheel;
- a **directional guide**: the aim walk takes the first point that is both far
  enough ahead AND not behind the car (cos > -0.35 ~= within 110 deg, so a
  perpendicular intersection turn still counts), and the joyride goal picker only
  accepts road samples inside a forward **70-degree** cone. Both exist to stop the
  car targeting something it has passed - and the tighter goal cone keeps the
  wander carrying on in the direction it is already driving instead of swinging
  back for a destination across or behind the road;
- **feelers** - short, FRESH `CellEmpty` probes with a deliberately chunky
  safe-zone radius (300 on the sides, 360 on the nose - wider than a car's
  half-width, so a thin pole between two rays is still caught) fanned in front of
  the car, which bias the aim around scenery the coarse (512-unit) and
  up-to-a-replan-old A\* grid cannot see. A wall on one side pushes the aim to the
  other; a street with walls both sides cancels out. The centre feeler brakes when
  the nose is genuinely blocked;
- a steering dead-band **with hysteresis** (start at 70, keep turning to 30) so
  it does not flip sides on noise, and a lift-off when a car is close ahead (no
  braking for traffic - that made the car crawl past oncoming vehicles);
- **tight turns on the L1 fast steer** (`CAR_PAD_FASTSTEER`): while a real heading
  error is being corrected (damped error over ~11 deg) the engine builds wheel
  lock at 64/frame up to +/-511 instead of 32/frame up to +/-352, so the car takes
  the corner instead of running wide. Gentle corrections keep the slower rate;
- a **stuck recovery** that backs out of a wedge (and asks the behavior for a new
  destination) after ~1.3 s at a standstill, plus an **early reverse** the moment
  scenery is right on the nose while barely moving (backing off sooner grinds
  less than waiting out the timer). **The steer bit in a burst is INVERTED**: while
  backing up, turning the wheel the way forward steering would swings the NOSE the
  other way, so the forward sign used to drive the nose *away* from the aim and
  every burst left the error worse than it found it - which is why a wedged car
  could sit in a reverse loop for fifteen seconds.

## Car dynamics (speed, grip, corners)

Walking a line is not driving: a car carries momentum and its velocity can point
somewhere other than its nose. Alleycat reads that (`ALLEYCAT_CARSTATE.slip`, the angle
between `st.n.linearVelocity` and the heading) and drives to a **grip budget**:

- `AlleycatScanCurve` finds the sharpest turn on the plan within `corner_lookahead`
  (its heading change, how far off it is, and the arc it happens over);
- `AlleycatSpeedTarget` turns that into a speed the car may carry now: the grip limit
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
| 2  | Evade   | wanders at a pace over the road limit and keeps clear of the police; runs flat out once they chase (see *Evade*) |
| 3  | Pursuit | chases another player's car in a session (see *Multiplayer: follow and flee*); with no target it wanders like Joyride |

Adding a behavior is one function (`CAR_DATA* -> pad`) plus one row in
`alleycat_behavior.c`'s table; the menu and config pick it up automatically.

## Multiplayer: follow and flee

**Pursuit** (`mode = 3`) is the behavior that cares about other players. Joyride
never chases anyone - it just drives around - so select Pursuit on the machine
that should tail the host. With a session live each machine plays its half:

- a **CLIENT** drives to the **host's car** and holds a **clearance**, so it tails
the host around the map while a match is being tested instead of ramming them;
- the **HOST** runs FROM the nearest other player, so the cars that joined to
follow have something to chase. Its bolt-hole is scored on the occupancy grid,
so it is a place the car can actually DRIVE to and not a straight line away
through a building.

On a **dedicated server** there is no host player: no roster row has id 0, so
`MpGetPlayer(0)` is NULL. There a client follows (or, with `flee_host`, runs from)
the **nearest** other connected player with a car instead. A listen server always
has its host row, so nothing changes there.

The session state comes from the SDK (`jer_net_is_active` / `jer_net_is_host`) and
the cars from the compiled-in `mp` module (`MpGetPlayer(i)->carId`); see
`alleycat_follow.c`. Both halves answer 0 when they do not apply, so single player,
and a session with nobody else in it, keep the plain random wander.

Within `follow_clearance` the car stops and holds instead of closing the last
stretch. That hold goes through `AlleycatDriveHoldPad`, **not** the steering pad, so
the stuck recovery that lives in the steering pad cannot mistake a
parked-on-purpose car for a wedged one and reverse into the host. Between the
clearance and twice the clearance the throttle is dropped, for a coast-and-brake
approach rather than a lunge.

`pursuit_ram = 1` (menu: *Pursuit: ram*) removes both of those: there is no
clearance hold and the throttle stays planted, so the car drives into the target
to ram it. The target is already excluded from the driver's traffic avoidance
(`followCarId`), so it never brakes for the car it is chasing.

`follow_car` is a **test lever**: it names a `car_data` index to follow with no
session at all, so the approach, the clearance and the idle can be exercised in
single player.

**Range: unlimited by default.** `follow_range` is 0, and 0 means *no limit* - the
chase is CHUNKED, not truncated. The A\* window is centred on the car and is only
~12,000 units across, so a host on the far side of a city cannot be planned to
directly. The goal is therefore **clamped to the window edge in the host's
direction** and re-planned as the car closes, which is what lets it cross a whole
map to rendezvous. Set a non-zero `follow_range` to make the AI give the target up
(and go back to wandering) once it is farther away than that.

A script does the whole thing for you: host a match as usual, then run
`tools/follow_host.py` (or `tools/follow_host.bat`) to bring in a joining follower.
It gives the follower its own run directory, so its log and `mp.ini` cannot
collide with the host's; `--clearance`, `--range`, `--yaw-lead`, `--ram` and
`--client-car` override the defaults, and `--dry` prints the command instead of
launching.

**Enable Alleycat only on a machine whose car nobody is driving**, here and on the
LAN: with a driving mode selected (any non-zero `mode`) Alleycat substitutes the pad, so on YOUR instance it would take
the wheel from you (set `mode = 0`, or turn Alleycat off in the mods menu). The
follower the script starts is configured for you, with the host's own port in its
`mp.ini` so the two cannot disagree about where the session is.

## What the planner will and will not route through

The A\* runs on an occupancy grid of `CellEmpty` probes - and **`CellEmpty`
deliberately ignores smashable scenery** (light posts, chairs, a lot of fencing),
so the grid used to route straight through props the car then grinds into.
`alleycat_map.c` now runs **both** tests: the engine's own, and the same one with the
smashable branch kept (`AlleycatMapCellHasSmashable`, shared with the steering
feelers). A sample with either in it is BLOCKED, and the planner never expands a
blocked sample - so it routes AROUND the scenery rather than through it.

The road network is a **preference, never a wall**: an off-road sample costs
`offroad_cost` more than an on-road one (90 vs the base 100), so the planner
takes the roads when they go the same way as the target, and still leaves them to
reach a goal that is not on one. At 0 there is no road preference at all.

## Measuring the steering loop

The once-a-second `[Alleycat] drive` line (written only while `steer_trace` is on)
**cannot show a saw**: it samples 30 frames
apart while the wheel is turned on and off in ones and twos. Set `steer_trace = N`
and the module instead logs a `[Alleycat] steer` line every N frames carrying the pad,
the heading error, the **damped** error the steer decision actually used, the feeler
bias, the speed, the corner-speed target, the **real front wheel angle** (`wa` - the
number that shows the engine integrating the pad), the look-ahead asked for vs where
the aim landed, the cross-track offset, the aim point, the plan's own bearing, and
the stuck/recover counters.

```
python JERICHO/MODS/Alleycat/tools/alleycat_steer_stats.py <log>
```

### What that measurement has established

A fixed-aim run (`aim_test = 0`, so the steering loop is driven by a *stable*
setpoint and nothing else) tracks cleanly: mean heading error in the tens, a wheel
angle around 41 (nowhere near the 352 lock), steering engaged 14% of the time,
**zero** direction reversals, and speed up to 181. So the pad law, the hysteresis
and the engine's 32/frame build vs 64/frame centring are **not** what makes the car
weave. Do not "fix" them on a hunch.

With a real destination the car instead runs a persistent heading error in the
hundreds (20-30 degrees) at partial-to-full lock while its cross-track error grows
against a plan that is usually a **2-waypoint straight run** (the goal only changes
every few seconds, so the line is stable, and `AlleycatStarLineClear` refuses any line
crossing a blocked sample, so the straight run is genuinely clear). The fault is
therefore in **the destination/aim the behaviour supplies**, not in the driver - and
`pdir` plus `ax`/`az` in the trace are what the next pass needs to pin it down.

## Running from the police

When the engine says the cops are after this car - `copsAreInPursuit` or
`CopsCanSeePlayer` - the police outrank EVERYTHING else, the follow included. The
car runs from the nearest `CONTROL_TYPE_PURSUER_AI` car, and unlike the other
behaviours it does so with the corner-speed model and the aggression dial bypassed
(`panicFlee` in the driver: `AlleycatSpeedTarget` returns the whole top speed), then
goes back to following or wandering the moment the pursuit ends.

`flee_police` (default 1) turns it off. **It needs `immune = 0`** - the default
since 0.2.0: immunity pins the wanted level at zero, so with it on no pursuit ever
starts and this can never trigger.

## Evade

`mode = 2`. A getaway driver that stays clean until it cannot:

- **Wander at a pace.** The same random road destinations as Joyride, but the
  target speed is capped at the road's own limit plus `pace_pct` percent. The
  limit is re-read under the car every frame (`roadLimit`: civ_ai.c
  `speedLimits[]` for the surface's `ROAD_SPEED_LIMIT`; a junction keeps the last
  road's).
- **Keep clear of cops.** A police car - a pursuer, a patrolling civ-AI cop
  (`CONTROL_FLAG_COP`), or a multiplayer cop player - inside `cop_clearance`
  world units is a threat: the car leaves for the reachable point furthest from
  it (the Joyride flee's bolt-hole, `AlleycatLocalFleeGoal`), still at the pace,
  because speeding past a cop is how a pursuit starts. It goes back to wandering
  once the cop is 1.25x the clearance away.
- **Lights.** `run_lights = 0` stops for a red - or an amber still more than ~600
  units off - on the exit the car is entering a junction by, read exactly the way
  the engine's red-light felony reads it (felony.c: `flags & 1`, the `ExitIdx`
  matching the road it is on, `junctionLightsPhase[exit & 1]`). Within 900 units
  it brakes to a stop; further out it coasts. `run_lights = 1` (default) never stops.
- **Steering cap.** `max_steer` caps the front wheel angle (stop adding lock at
  the cap). Alleycat steers at the regular D-pad rate, which the engine tops out at
  352, so the default 512 is no cap. It is a bang-bang cap on a digital pad (the
  wheel settles between the cap and the cap minus one centring step), and it is
  never applied to the wall-evasion turn or the reverse burst.
- **Chased.** Once `copsAreInPursuit` / `CopsCanSeePlayer` is set it is the police
  flee above: flat out, no pace, no lights, longer look-ahead.

The once-a-second drive line carries `evade=` (a cop inside the clearance),
`cop=` (distance to the nearest police car), `limit=` / `cap=` (road limit and
the cruise cap) and `light=` (holding for a light).

A closing pursuer covers `replan_move` almost instantly, so the plain "the threat
moved" trigger used to redraw the bolt-hole every ~12 frames. A destination
redrawn 2.5 times a second is never DRIVEN TO: the aim swings, the heading error
stays huge, and the car reads as lazy while it thrashes. The police case now waits
**six times** as long before reconsidering - a fleeing car commits to a direction.
Measured effect: goal changes fell from 34 to 18 over a 700-frame survival run.

### Being chased: letting go of the road

Under a chase the pull back to the road is a liability - a car that keeps snapping
onto the road loses whatever line it had, and its speed with it. Two knobs answer
that, both applied only while panicking:

- `panic_offroad` (default 30, against the normal `offroad_cost` 90) weakens the
  road preference, so the planner will take a direct line across open ground;
- `panic_lookahead` (default 1600) is ADDED to the aim's look-ahead, so the aim
  sits much further down the road and the car commits to the line instead of
  correcting short and often.

Measured: the look-ahead does take effect (la rose from ~2194 to ~3508 in a
survival run). What has NOT changed yet is the headline number - the heading error
still averages ~950 game units and the car still sits at full lock over half the
time - so these knobs are necessary but not sufficient. The remaining cause is the
one the steer trace points at everywhere: the destination/aim the behaviour picks,
not the driver's hold on it.

### Wheelspin, and why it is currently inert

`panic_wheelspin` (default 1) lets the car use the engine's wheelspin (the Circle
bind) rather than plain throttle to get off the line: `CAR_PAD_WHEELSPIN` makes
wheelforces apply maximum thrust regardless of the pedal. It pays for that by
loading the front tyre force and starving the rear (`*rearFS = 0x780 - *frontFS`),
so a wheelspun car has little rear grip and spins out the moment it is asked to
turn - which is why the guard is the FRONT WHEEL ANGLE and the heading error,
not the pad bits (the bits lead the wheel by a frame or two, so requiring "no
steering bit" as well asked for something that almost never holds).

**Measured: it fired on 0 of 110 sampled frames** in a HAVANA survival run. The
car's heading error averaged ~966 game units (about 85 degrees) and it sat at full
lock half the time, so it was essentially never straight and accelerating and the
guard never opened. That is the same fault the steer trace found everywhere else:
the car cannot track the destination it is given, so it spends its life at full
lock wrestling a bearing it never reaches - and anything that rewards being
straight (a clean launch, real corner-exit speed) never happens. The wheelspin
path is kept because it is correct and guarded, but it is **unverified in
practice** until the aim problem is fixed.

## Visualization (`viz_*`)

Drawn in `JER_EVENT_DRAW_WORLD` with `Debug_AddLineDepth` (buildings occlude the
lines):

- `viz_road` — the engine road graph: nodes as crosses (yellow straight / orange
  curve / green junction) and the `ConnectIdx` edges as blue lines. The graph is
  built once per level as soon as this layer is on (before 0.2.0 only a dump
  built it, so the layer drew nothing);
- `viz_grid` — the occupancy grid: blocked samples (walls) in red, road samples
  in green, within ~6,000 units of the car;
- `viz_path` — the current A\* plan (red), the goal cross (yellow) and the aim
  line (green);
- `viz_hud` — up to four short lines in the top-left corner (see *HUD*).

## HUD

While `viz_hud` is on (the default) and Alleycat is enabled, the top-left corner
shows up to four lines, refreshed three times a second:

1. `Alleycat <behavior>` and what it is doing (following/ramming, fleeing, leaving
   cop, CHASED, red light, reversing). Cyan for Joyride/Pursuit, amber for Evade,
   red while chased. In Manual this is the only line: `Alleycat Manual  (L2: settings)`.
2. `Speed <now> / <target>`, plus Evade's cruise `cap`.
3. `Route <n> pts, goal <distance>` (`(part)` while the plan is a partial route).
4. The situation: nearest cop and the road limit (Evade), distance to the car
   being followed or fled from, or the corner ahead and the grip.

The lines are `jer_hud_panel` slots 6-9 (the owner table is in `jer_hud.h`), not
the shared 4-line notice queue, so they never push out anyone else's messages and
are never cut short. Before 0.3.0 this was one ~150-character notice a second, cut
at 96 characters and fighting mp's lines for the same four rows.

## Log

Quiet by default. Alleycat writes its boot lines (`registered`, `run seed`, the road
graph counts) and then one `[Alleycat] state (...)` line whenever the drive changes:
on/off, behavior, following, fleeing, chased, cop near, reversing out of a wedge.

With `steer_trace` on (see *Measuring the steering loop*) it also writes the
35-field `[Alleycat] drive` line once a second, the `[Alleycat] steer` line every N
frames and the first three pad probes. Before 0.3.0 the drive line was written
every second whether you wanted it or not.

## Immunity

While Alleycat is driving (and `immune = 1`; the default is 0 since 0.2.0, so the
police can chase) the player's car is
invulnerable: `JER_EVENT_GET_BUDDHA` clamps its damage below the totaled
threshold, and the frame hook keeps the wanted level at zero
(`felonyRating`/`copsAreInPursuit`/`CopsCanSeePlayer`/`pedestrianFelony`), so no
cop sirens chase the test drive. Manual play (`mode = 0`) is left untouched.

## Settings page (L2)

Alleycat's settings are a page of the game's own pause menu (`jer_pause_menu.h`,
id `alleycat`): Continue → JERICHO Addons → Alleycat. **L2** opens the pause straight on
that page (`jer_pause_menu_open`), only while Alleycat is enabled. Triangle or Start
on the page closes the pause; *Close* does too.

| Row | What it does |
|-----|--------------|
| Behavior: < Manual / Joyride / Pursuit / Evade > | left/right or Cross |
| Immunity | `immune` |
| Follow players | `follow` |
| Pursuit: ram | `pursuit_ram` (no clearance hold; drive into the target) |
| Flee players | `flee` |
| Evade settings | submenu: *Pace* (`pace_pct`, ±5), *Red lights* Run/Stop (`run_lights`), *Cop clearance* (`cop_clearance`, ±500), *Max steer* (`max_steer`, ±32, "no cap" at 352+), *Road limit* (read-only: the limit of the road under the car and the cruise Evade makes of it), *Back* |
| Road view | `viz_road` |
| Path view | `viz_path` |
| HUD lines | `viz_hud` |
| Steer trace | Cross: off / every 3 frames; left/right: 0-30 (`steer_trace`) |
| Dump road graph | writes the graph (see *Road-graph dump*) once the game runs again |
| Close | back to the game |

Every change is written to `Alleycat.ini` at once. The pause stops the game, so the
car gets no input while the page is open; the key that closed it (Cross, Triangle,
Start, L2) is kept off the car until it is released, so closing never accelerates,
leaves the car or cycles the control mode. `viz_grid` has no row (it is heavy);
set it in the ini. L2 is still the camera's look-left (and Caine's Crossfire's
gun): those read the pad outside the car-pad hook. Before 0.3.0 the menu was a
bare text list drawn over the game that ran off the bottom of the screen and
showed `> > Joyride` (the label and the cursor both drew a `>`).

## Road-graph dump

Set `dump_now = 1` (or pick *Dump road graph now*) and the module writes, into
the working directory:

- `Alleycat_roadgraph.json` — nodes (surfId, kind, world x/y/z, lanes, speed limit)
  and edges (node index pairs);
- `Alleycat_roadgraph.obj` — the same graph as geometry (world/4096, X mirrored).

## Layout

| File | Purpose |
|------|---------|
| `alleycat.c/.h` | module entry, config, the CAR_PAD injection and the HUD |
| `alleycat_internal.h` | shared state + file map (glue between this module's files) |
| `alleycat_map.*` | the occupancy grid (engine probes: `CellEmpty`, `JerRoadAt`, `MapHeight`) |
| `alleycat_mapgrid.c` | the pure grid queries + cost model |
| `alleycat_star.*` | A\* over the grid |
| `alleycat_local.*` | the local search ("get back to a road") |
| `alleycat_road.*` | the engine road graph (surfaces + `ConnectIdx`) as nodes/edges |
| `alleycat_dump.c` | the offline road-graph export |
| `alleycat_drive.*` | pad injection + waypoint following |
| `alleycat_behavior.*` | the modular behavior table |
| `alleycat_follow.*` | who the AI chases in multiplayer (the mp host, or the nearest player on a dedicated server) + the clearance; the police Evade keeps clear of |
| `alleycat_viz.c` | debug drawing |
| `alleycat_menu.c` | the settings page (pause menu) and the L2 key |
| `tools/follow_host.*` | join a match on localhost as an extra player that follows the host |
| `tools/alleycat_steer_stats.py` | summarise a `steer_trace` log (the steering loop's own numbers) |

The search library (`alleycat_map` / `alleycat_mapgrid` / `alleycat_star` / `alleycat_local`)
is a namespaced copy of `mp`'s `ai/` — both modules are compiled into the same
exe, so the symbols must not collide. The pure half is exercised engine-free by
`src_rebuild/Game/C/JERICHO/test/test_alleycat_path.c`.

## Tunables (`JERICHO/CONFIG/Alleycat.ini`, namespace `[Alleycat]`, read live)

See `alleycat.h` for the key names and defaults. The car-dynamics knobs:

| key | default | meaning |
|-----|---------|---------|
| `corner_g` | 2 | lateral-g budget; bigger = more speed through the same curve |
| `brake_gain` | 1 | braking authority for the braking-distance term |
| `corner_lookahead` | 6000 | how far ahead the plan is scanned for corners (world units) |
| `speed_scale` | 100 | overall aggression: the corner target as a percentage |
| `avoid_dist` | 1200 | start avoiding a car ahead within this range (**capped at `ALLEYCAT_AVOID_MAX` = 1200**; a larger value is logged, not silently ignored) |
| `steer_trace` | 0 | log a compact steering-loop line every N frames (0 = off); while on, the once-a-second `drive` line is logged too - see *Measuring the steering loop* and *Log* |
| `yaw_lead` | 4 | steering damper: frames of the car's own yaw the aim leads by. **Lower steers harder**; higher straightens the wheel earlier (the old fixed value was 8, which read as steering far too slowly) |

`brake_gain` is in speed^2 per world unit; 40 swamps the term (the target never
drops), so keep it near 1.

The multiplayer follow/pursuit knobs (see *Multiplayer: follow and flee*):

| key | default | meaning |
|-----|---------|---------|
| `follow` | 1 | Pursuit chases the host's car while a session is live (0 = never) |
| `follow_clearance` | 1500 | world units held from the followed car before stopping |
| `follow_range` | 0 | give the target up past this distance; **0 = unlimited** (a chunked chase across the whole map) |
| `pursuit_ram` | 0 | 0 = hold `follow_clearance`; 1 = no hold, drive into the target to ram it |
| `follow_car` | -1 | TEST LEVER: a `car_data` index to follow with no session (Pursuit mode) |
| `flee` | 1 | while the HOST in a session, run from the nearest other player (0 = never) |
| `flee_police` | 1 | run from the police FLAT OUT when they are after us (needs `immune = 0`) |
| `panic_wheelspin` | 1 | may use the wheelspin launch while fleeing the police (straight-line only) |
| `panic_offroad` | 30 | the road preference to use WHILE CHASED, replacing `offroad_cost` (0 = take the direct line) |
| `panic_lookahead` | 1600 | added to the aim's look-ahead while chased, to hold speed down a longer line |
| `flee_car` | -1 | TEST LEVER: a `car_data` index to run from with no session |
| `offroad_cost` | 90 | extra A* cost for an off-road sample (on-road is 100); 0 = no road preference |

The Evade knobs (see *Evade*):

| key | default | meaning |
|-----|---------|---------|
| `cop_clearance` | 6000 | a police car inside this (world units) is a threat to leave; released at 1.25x |
| `pace_pct` | 25 | cruise pace over the road's own speed limit, in percent (ignored once chased) |
| `run_lights` | 1 | 1 = never stop at lights; 0 = stop for red/amber unless chased |
| `max_steer` | 512 | front wheel-angle cap; 352 or more = none (the engine's regular lock) |
| `top_speed` | 255 | overall speed cap, every behavior |
| `immune` | 0 | while driving: no damage, wanted level pinned at 0 (blocks every police chase) |

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

Enable it from Options → JERICHO (mods menu), or set `Alleycat = 1` in
`JERICHO/CONFIG/modlist.ini`.
