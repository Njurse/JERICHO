# Combat D2 opponent AI

The opponent cars you fight in combatd2. The brain is `ai/opponent.c` (the large
file); `ai/nav.c`, `ai/grid.c` and `ai/flow.c` are the navigation layer it steers
with. Line references are into `JERICHO/MODS/combatd2/` unless stated otherwise;
where a number is quoted below it was read off the source, not a comment.

Nothing else in the engine drives these cars: they are spawned as
`CONTROL_TYPE_CUTSCENE` cars (opponent.c:568) and their controls are written
directly (`cp->thrust` / `cp->wheel_angle / cp->handbrake / cp->wheelspin`,
opponent.c:1587-1590) through the **same combatd2 handling path the player uses**,
so they feel like the player's car. There is no stock traffic AI on them and no
road-node snapping / `CheckPingOut()` (see the note at opponent.c:497-500).

Two file-top comments are stale: `ai/ai.h:4-5` and `opponent.c:8-13` describe the
states as HUNT / FLEE / RECOVER / WANDER (+ an EVADE overlay). The real state set
is the `CD2_AI_*` enum in `combatd2.h:395-404` (see §3). EVADE is not a state at
all — it is an overlay field (see §7).

---

## 1. What it does, and how it is ticked

`cd2AiRegister` (opponent.c:2042-2053) is called from `cd2Register` at
combatd2.c:1935, last of the merged sub-modules. It registers seven JERICHO hooks.
JERICHO priority is **lower runs first** (`sdk/include/jericho.h:172`):

| Hook | Prio | Handler | Purpose |
|---|---|---|---|
| `JER_EVENT_FRAME` | `1` | `cd2AiOnFrame` (1699) | build nav graph, one-shot probes, reap dead opponents, respawn |
| `JER_EVENT_GAME_START` | `0` | `cd2AiOnGameStart` (1834) | wipe the AI slots |
| `JER_EVENT_CAR_STEP` | `1` | `cd2AiOnCarStep` (1816) | run `cd2AiDrive` for each opponent |
| `JER_EVENT_CAR_PAD` | `-1` | `cd2AiOnCarPad` (1792) | blank the pad the engine fed the car |
| `JER_EVENT_DRAW_MAP` | `0` | `cd2AiOnDrawMap` (2001) | plot opponents on the map, colour by role |
| `JER_EVENT_DRAW_OVERLAY` | `1` | `cd2AiOnOverlay` (1927) | on-screen "what is it thinking" readout |
| `JER_EVENT_DRAW_WORLD` | `2` | `cd2AiOnDrawWorld` (1903) | nav-graph debug draw (`nav_debug`) |

Two of those priorities are deliberate:

- **CAR_PAD at -1** runs *before* combatd2.c's own CAR_PAD (registered at 0,
  combatd2.c:1910) and d2pl's. The handler captures what the engine handed the
  opponent (`sDbg.padIn`), then forces `pad = 0`, `padSteer = 0`,
  `useAnalogue = 0`, `handled = 1` so the stock pedal path never assigns controls
  (opponent.c:1805-1811). A CUTSCENE car is fed `cjpPlay` stream 0 = the player's
  live replay, so without this the opponent would mirror the player's input.
- **CAR_STEP at 1** runs *after* combatd2wreckfx.c's CAR_STEP (-1) and
  combatd2sim.c's (0). So the AI writes its inputs last,
  on top of everything the core does.

The drive itself runs on **CAR_STEP**, which is one per `StepCars` — the 30 Hz
simulation step (see the note at combatd2.h:386-388). Both the FRAME handler and
the CAR_STEP handler bail unless the match fields opponents
(`cd2MatchOpponents() > 0`, 1704, 1823).

What the FRAME handler does (1699-1790), in order:

1. `cd2NavReady()` — build the road graph once the level's road lumps are
   resident (GAME_START can fire before they load).
2. One-shot arbitration probe (`sNavProbeDone`): asks the router for a route to
   a point 7000 x / -5000 z off the player and logs which source won (road vs
   off-road), plus four standalone `cd2GridPath` probes (1714-1761). Diagnostic
   only — `carId` -1 so no car's route cache is touched.
3. Reap: any slot whose `carId >= MAX_CARS` or whose car is
   `CONTROL_TYPE_NONE` is cleared (1763-1781).
4. Respawn: if zero opponents are live, `cd2AiSpawn()` (1785-1786). This is the
   only place opponents are created. Spawning waits for a player car to exist
   (`cd2WpnPlayerCar`, 651) — hence the FRAME-tick, not GAME_START.

`cd2AiOnGameStart` only resets `sAi[i].carId = -1` and the count (1840-1844).
`cd2AiActive` returns `sAiCount > 0` (1868).

---

## 2. Roles

Four archetypes, `CD2_AI_ROLE_*` (combatd2.h:409-416), named by
`cd2AiRoleNameOf` (1851-1859):

| Idx | Enum | Display | What it does |
|---|---|---|---|
| 0 | `CHASER` | "Chaser" | default: approach the target at a stand-off (see below) |
| 1 | `FLANKER` | "Flanker" | ATTACK goal shifted **2600** units along the target's right vector (opponent.c:973-978) |
| 2 | `AMBUSHER` | "Ambusher" | ATTACK goal shifted **5000** units along the target's forward vector, i.e. get ahead and lie in wait (opponent.c:979-984) |
| 3 | `HARVESTER` | "Harvester" | "roam for pickups/caches (rally points for now)" |

The role only bends the *goal the navigator is pointed at*. It is read in exactly
three places: the ATTACK-goal offsets above (only FLANKER/AMBUSHER are
special-cased — see the gotcha in §9), the map-blip colour (2024-2030), and its
name. **HARVESTER currently does nothing distinct**: it takes the same default
stand-off approach CHASER does; only its name and map colour differ (the enum
comment "rally points for now" matches the code).

**Assignment at spawn** (opponent.c:611-614):

```c
A->role = (gCd2Cfg.aiRole >= 0) ? gCd2Cfg.aiRole
        : (int)(((unsigned int)index + cd2AiRunSeed()) % CD2_AI_ROLE_COUNT);
```

So `ai_role = -1` (default) round-robins the four opponents by spawn `index`,
rotated by a per-run offset so the same car isn't always the same role; setting
`ai_role` 0-3 forces one role for all of them. `cd2AiRoleOf` maps a CAR_DATA back
to its role, or -1.

---

## 3. State machine

The states are `CD2_AI_*` (combatd2.h:395-404) and their display names come from
`cd2AiStateName` (1975-1997): `Auto, Disperse, Roam, Attack, Flee, Recover`.
`Auto` (0) is not a state — it is "let the AI pick" (`aiForceState`).

Selection (opponent.c:828-895) runs only when the per-car hold timer and state
timer allow, and only when `aiForceState == CD2_AI_AUTO`:

- `sHold > 0` → hold the current behaviour (dwell), don't re-decide.
- `--sStateTimer <= 0` → re-decide. Priority order:

| Condition | → State |
|---|---|
| `sDisperseTicks > 0` | `DISPERSE` (opening move) |
| `sFleeCooldown <= 0 && (totalDamage > fleeDamage \|\| danger >= fleeThreats)` | `FLEE` |
| `sRoamTicks > 0 && target not within `ROAM_INTERRUPT`` | `ROAM` (deliberate travel leg) |
| `targetId >= 0 && targetD2 < engage²` | `ATTACK` |
| else | `ROAM` |

`engage` is `ENGAGE_KEEP` (11000) while already in ATTACK, else `ENGAGE_RANGE`
(10000) — hysteresis so it doesn't flip in and out of the fight each frame
(opponent.c:844, 874-882). On a change, `sHold = MIN_STATE_TICKS` (150); the new
`stateTimer` is `STATE_TICKS(145) + cd2AiRand(60)`.

Timers counted down every tick, independent of re-decision
(opponent.c:902-925):

- `disperseTicks` — frames left of the opening spread (`DISPERSE_TICKS` = 420 at
  spawn, + up to half again jittered, line 623).
- `roamTicks` — frames left of the roam break-off (`ROAM_TICKS` = 300, + random
  `ROAM_JITTER` = 90; set when an ATTACK burst ends).
- `engageTicks` — consecutive frames in ATTACK; when it passes `ENGAGE_TICKS`
  (4500) it resets and starts a roam leg (`ROAM_TICKS`, line 912-920).
- `fleeCooldown` — set to `FLEE_COOLDOWN` (900) + jitter when it enters FLEE, so
  a once-battered car doesn't run forever (damage never heals).

Health / stuck / idle handling (opponent.c:721-944):

- `totalDamage` only ever rises, so an increase means a fresh hit; `sHits` counts
  them (723-726).
- A totaled car (`cd2CarTotaled`, opponent.c:735) is fully released: thrust 0,
  steer 0, handbrake 0, pivot 0, and drive returns. The debug snapshot reports
  `CD2_AI_RECOVER` at that point (748) — this is the only place "Recover" is
  produced in normal play.
- Near-standstill for `IDLE_TICKS` (200) frames → bump `wanderHeading`, drop the
  destination, reverse out (`REVERSE_TICKS` = 12) (934-944).

The forced-state path (`aiForceState != AUTO`, 829-832) sets `sState` directly
and skips the selector. Note it never *adds* behaviour: there is no
`DISPERSE`/`RECOVER` branch in the navigator (§5), so forcing those behaves like
ROAM. See §9.

---

## 4. Per-run seeding

**Why the engine RNG can't be used.** `Random2` (`src_rebuild/Game/C/convert.c:184-187`):

```c
int Random2(int step)
{
	return RAND((CameraCnt - frameStart) * (CameraCnt - frameStart)) >> 8 & 65535;
}
```

with `#define RAND(seed) ((seed) * 0x19660D + 0x3C6EF35F)` (dr2math.h:42). Two
facts, both true as written:

1. **`step` is ignored** — the result is a raw 16-bit value regardless of the
   range asked for. Anything needing "jitter up to N" must reduce it itself.
2. **It is a pure function of the frame index** `CameraCnt - frameStart`. Within
   a frame every call returns the same value, so anything driven only by it
   replays identically every launch (the opponents spawn on the same frame each
   boot).

Both claims are confirmed by the source, and are the reason for the module's own
RNG.

**`cd2AiRunSeed()`** (opponent.c:224-254) returns a value that varies per launch.
On first call it XORs: the address of its own static (`ASLR`), a stack address
(`ASLR`), `__rdtsc()` (MSVC x86/x64 only — see the `int.h` note at 44-52; *not*
`<time.h>`, which the game's include path shadows with its own `Game/C/time.h`),
`Random2(0) << 11`, then an avalanche `s = s*2654435761u + 2246822519u`. The
result is cached for the process (`sDone`). It only feeds the RNG at init and
rotation offsets (nav roam scan, role round-robin) — it is not re-read per frame.

**`cd2AiRngNext`** (256-269) is a Numerical Recipes LCG:
`state = state*1664525 + 1013904223`, seeded once from `cd2AiRunSeed()`.

Two consumers of that LCG:

- **`cd2AiRand(n)`** (271-277): `(next() >> 16) % n`. The general "random in
  [0,n)" used throughout drive logic (jitter windows, headings, timers).
- **`cd2AiRandSalt(n, salt)`** (291-301): `((next() >> 16) ^ (salt *
  2654435761u >> 7)) % n`. The salt exists because **all opponents spawn on the
  same frame** — a plain `cd2AiRand` for each would still draw different values
  (it's a running LCG, not per-frame constant like `Random2`), but the salt
  decorrelates the opening model/palette picks so they don't collide and every
  opponent doesn't come out the same colour. It is used only at spawn
  (model choice, 558; palette, 566).

Both advance the same single LCG, so the spawn picks interleave deterministically
given the run seed.

---

## 5. Navigation (`ai/nav.c`, `ai/grid.c`, `ai/flow.c`)

The navigator (opponent.c:946-1209) picks a world-space `goalV`, asks
`cd2NavRoute` for a polyline, then steers along it by pure pursuit. Three
subsystems contribute, arbitrated inside `cd2NavRoute`.

### nav.c — road graph + A*

A graph over the level's Driver 2 road segments in one node index space
(`nav.c:1-10`): `[0..NumStraights)`, then curves (`surfId 0x4000|i`), then
junctions (`0x2000|i`). Edges come from each segment's `ConnectIdx` (via
`GetSurfaceRoadInfo`), made **undirected** so a route traverses either way
(`cd2NavAddEdge` scans for an existing link first, 314-334; each node keeps up to
4 neighbours). Node positions are the segment **midpoints**; junctions have no
midpoint so they take the **average of their (already positioned) neighbours**
(416-443). Node `y` is `MapHeight` cached at build (448-459).

Capacities (`nav.h:15-21`): `MAX_ROUTE` 64 waypoints, `WP_STEP` 512, `MAX_NODES`
4096, `MAX_CARS` 32 route-cache slots, `DRAW_RADIUS` 4500.

Routing (`cd2NavRoute`, nav.c:727-916):

- Start = exact surface node (`cd2NavNodeAt`) else nearest road node within
  **2048**; goal node similarly within **1500**.
- Cached per `carId`; re-planned only when the goal node or goal position changes
  (`|Δx|+|Δz| >= 1024`, 762-764).
- Road A* first (`cd2NavSearch`, 641-721). Edge cost is `dist * (4 -
  speedLimit)`, heuristic is admissible Manhattan distance (558-574).
- Off-road fallback (804-826): if the road route failed, **or** the straight-line
  distance is `> 1200` and the road route is more than **2x** the straight line,
  run `cd2GridPath` instead.
- If neither works → a **single direct waypoint** at the goal, source
  `CD2_NAV_SRC_DIRECT` (833-850). The comment (835-845) says this is deliberate:
  near intersections and when the goal is right on top of the car both routers
  legitimately fail, and returning nothing used to make the AI give up mid-chase.
- The result is **densified** (`cd2NavDensify`, 198-275): legs are subdivided so
  no leg exceeds `WP_STEP`, and any leg whose straight line is not `lineClear` is
  bent around the obstruction via `cd2NavRepairLeg` (161-196) — it tries six
  perpendicular offsets (±400/800/1400) and keeps the first that is clear both
  ways. Densifying matters because steering at sparse segment midpoints makes the
  car cut the corner into whatever is on the inside of the bend (146-152).

`cd2NavRoamGoal` (nav.c:96-144) picks a road node to cruise toward:

- `minDist`/`maxDist` filter by 2D distance; among acceptable nodes it returns the
  **farthest** (not the first in index order — "index order follows the road
  layout", which kept handing back the same corner of the map, 113-116).
- Scan starts at a **per-run rotating index**, seeded once per run:
  `sRotate = cd2AiRunSeed() % sNodeCount` (107-108), then advanced to `best + 1`
  each call. This is the "seeded offset" — it makes the opening roam route differ
  between launches.
- Returns 0 when no node fits (the caller then wanders — see below).

`cd2NavDraw` draws nodes (vertical ticks, coloured by type) and edges around a
centre (932-982).

### grid.c — off-road drivability A*

A bounded window (bbox of from/to + margin), rasterised on demand at
`CD2_GRID_CELL` = 512 units, at most 64×64 cells (`grid.c:17-21`). Per cell:
`MapHeight`, an on-road flag, and `CellAtPositionEmpty(p, 160)` for scenery
clearance (114-137). Neighbour passability requires both cells clear, a height
step ≤ `MAXSTEP` (700), and `lineClear` between the centres (142-159). Octile A*
(`cd2GridPath`, 233-463); on-road cells get a **250** cost bonus vs off-road
(395). Start/goal cells that are blocked are nudged to a clear neighbour
(281-335). Waypoints are sampled one cell apart (subsampled if too many) and the
true goal is appended (435-460).

### flow.c — shared pursuit field

A 96×96 window of 256-unit cells holding a fast-marching distance to a moving
goal (`flow.c:19-28`). `cd2FlowSetGoal` re-centres the window when the goal moves
more than `CD2_FLOW_RECENTRE` (2500) and otherwise re-seeds only when the goal
enters a new cell, throttled to `RESEED_EVERY` (8) frames (151-192) — a
fast-moving goal would otherwise reset the field every frame. `cd2FlowUpdate`
propagates up to `budget` cells/frame (194-235); diagonal step 362, straight 256,
blocked cells (`CellAtPositionEmpty(p, 120)`) skipped. `cd2FlowDir` reads the
lowest-distance neighbour cell and returns the heading downhill (247-299), giving
any number of opponents an O(1) "which way to the goal" with no per-car search —
costing nothing per car is what lets a pack converge cheaply.

### How drive ties them together (opponent.c:946-1209)

Goal by state: ATTACK → target ± role offset ± stand-off; FLEE → `cd2NavRoamGoal`
with `FLEE_RUN_MIN..MAX` (30000..150000), else a mirrored escape from the crowd,
else the disperse leg; DISPERSE → wanderHeading × `DISPERSE_LEG`; ROAM → the
nearest target if one is on the board (the "converge" half), else a stored road
goal (re-picked every `GOAL_TICKS` 1800, or when within `WP_REACH` 700), falling
back to a `WANDER_LEG` (40000) heading when `cd2NavRoamGoal` returns 0.

Then, every opponent every tick:

- `cd2NavRoute(cp->id, &carV, &goalV, &sRoute)` (1088) — A* + grid, cached.
- `cd2FlowSetGoal(&goalV); cd2FlowUpdate(64);` (1091-1092). **This runs per
  opponent**, so with 4 opponents the field advances up to 256 cells/frame, not
  64. (The `CD2_FLOW_BUDGET` 64 define is bypassed here — the literal is 64.)

Steering heading (1108-1188): while evading, a quarter-turn away from the threat;
otherwise **pure pursuit** along the route polyline at a speed-scaled lookahead
(`LOOKAHEAD_MIN` 1400 + speed×`LOOKAHEAD_PER_SPEED` 12, capped at
`LOOKAHEAD_MAX` 6000). If no route, fall back to the flow field, then to a direct
`ratan2` at the goal.

---

## 6. Spawning and vehicle choice

`cd2AiSpawn` (646-663) resets the slots then calls `cd2AiSpawnOne` for
`i = 0 .. min(CD2_AI_MAX, CD2_AI_SPAWN_COUNT)-1`. Both are **4** (opponent.c:176-177),
so up to four opponents.

`cd2AiSpawnOne` (440-644):

1. Find the first `CONTROL_TYPE_NONE` car slot (454-461); bail if none.
2. **Placement.** Probe both sides at this opponent's fan distance
   `off = SPAWN_OFFSET(900) * (index + 1)` (then 900 further out, up to 5 tries),
   using the player's orientation columns, and spawn on whichever side is
   `lineClear` (471-490). Boxed in → retry next frame.
3. **Vehicle choice — the important part** (512-576). `InitCar`'s model argument
   is a **resident slot index** (0..`MAX_CAR_RESIDENT_MODELS`-1), not a global
   model id. A level need not use every slot, and an **unused slot has NULL model
   pointers which fault the moment the car is drawn or dented**. So it
   *enumerates* the usable slots rather than guessing one at random:

   ```c
   for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)
       if (gCarCleanModelPtr[i] != NULL && gCarDamModelPtr[i] != NULL &&
           gCarLowModelPtr[i] != NULL && i != pcp->ap.model)
           loaded[n++] = i;
   ```

   Three models must all be present, not just clean: `CreateDentableCar` also
   needs the low-detail model and bails with `gCarLowModelPtr is NULL`, after
   which the half-built car is dereferenced and the game dies (comment 518-522).
   The engine's own traffic spawner guards on exactly this
   (`civ_ai.c:2225`: `if(!gCarCleanModelPtr[model]) return 0;`). The extra
   `i != pcp->ap.model` **deliberately skips the player's own model** so opponents
   aren't visually identical to the player (that, and the NULL-slot fault, are why
   "every opponent previously came out as the player's car" — comment 511).
   The chosen slot is `loaded[cd2AiRandSalt(n, index+1)]`, or the player slot
   `pcp->ap.model` when `n == 0` (the level loaded only one usable car, 555-558).
   `MAX_CAR_RESIDENT_MODELS` is 8 on PC (`src_rebuild/Game/dr2limits.h:22-34`),
   5 on PSX.
4. **Palette** (560-566): recolourable civ bodies (`residentCarModels[model]` in
   1..4) get `cd2AiRandSalt(6, index*31+17)`; single-palette bodies (cop /
   special) get 0. A CUTSCENE car *does* take `InitCar`'s palette argument — only
   CIV_AI overrides it from `extraData`, which is why the prototype's cars never
   varied before.
5. `InitCar(slot, pcp->hd.direction, &pos, CONTROL_TYPE_CUTSCENE, model, palette,
   &cd2AiPadId)` (568); `cd2AiPadId` is a static 0 (a CUTSCENE car with no replay
   stream).
6. Claim a free AI slot and initialise it (579-641): state `DISPERSE`,
   `disperseTicks = DISPERSE_TICKS + cd2AiRand(DISPERSE_TICKS/2)`,
   `wanderHeading = car direction + index*1000 + cd2AiRand(600)` (mod 0x1000), and
   the disposition below.

**Disposition** (`cd2AiBravery`, 318-330): `bravery` 0-100 = average of
`mass*50/MASS_REF(1200)` and `MaxDamage*50/HEALTH_REF(20000)`, clamped. It falls
out of the car data the engine already has rather than a per-model table. From it
(630-631):

- `fleeDamage = MaxDamage * (20 + bravery*80/100) / 100` — the damage at which
  this car breaks off.
- `fleeThreats = 5 - bravery/25` — the number of close threats at which it breaks
  off (5 = timid, 1 = brawler).

The model pool for the level is logged once (`sPoolLogged`, 530-553) — this line
is what tells you which `-car` ids are legal in the city being tested.

---

## 7. Driving, avoidance, combat

All inside `cd2AiDrive` (665-1695); one `CD2_AI_CAR` slot per opponent
(`sAi[CD2_AI_MAX]`, 207), so four run independently.

**Evade overlay** (773-784, 1109-1115). `cd2WpnIncomingThreat(cp, ...)` (a
weapons-module call, weapon_internal.h:111) sets `sEvade = EVADE_FRAMES` (85,
~1.5 s) and it decrements each tick. While active it steers a quarter-turn
(`±1024`) to the side the threat is **not** on, suppresses firing, and holds off
the idle counter. It rides on top of whatever base state is active.

**Scenery fan** (1216-1279): five rays at offsets `{900,450,0,-450,-900}` from
the heading, each sampled in `FAN_STEPS` (10) steps of `LOOK/FAN_STEPS`
(`4500/10` = 450). The **measured clear length** per ray (`freeRun[]`) drives
everything: `freeAhead/freeL/freeR`, and `nearBlocked`. `nearBlocked` is true
when `freeAhead < need` (where `need = speed*STOP_FRAMES(8)`, capped to the fan's
reach) **or** `freeAhead < NEAR_BLOCK_MIN` (350). The cap matters: without it a
large `STOP_FRAMES` demands more clearance than the fan can see, so the test is
permanently true and every opponent drives on the brakes.

**Imminent-collision response** (1281-1327), held a few frames for hysteresis
(`avoidTicks`). Order of preference: swerve (normal steering) → brake → pivot in
place → reverse. Cases: near-zero room → reverse and drop the destination;
≥2 consecutive activations → reverse; speed > `BRAKE_SPEED` (360) → brake
(`AVOID_BRAKE`, 20 frames); else pivot (`AVOID_PIVOT`, 12 frames).

**Stuck detection** (1329-1364): commanded forward but `|speed| < STUCK_SPEED` (5)
for `STUCK_TICKS` (70) → reverse `REVERSE_TICKS` (12). Reversing deliberately
drops the destination and re-aims the wander heading so it doesn't back into the
same wall.

**Control output** (1369-1590): highest-priority branch wins. Reverse: thrust
`-TMB_THRUST`, steer toward the target. Avoid-pivot: `pivotDir * STEER_MAX`,
thrust 0, pivot via `cd2CarSetAiPivot`. Avoid-brake: hard brake + full steer.
Acute heading error (`|diff| > PIVOT_DIFF` 1150) below `PIVOT_SPEED` (150) →
stop and pivot; above → brake-in and pivot at speed. Otherwise normal steering:

- `steer = diff / (STEER_DIV(6) + |speed|/STEER_DIV_SPEED(55))` — the gain *falls*
  with speed, or a fixed gain overshoots every frame and weaves (1419-1422).
- Swerve away from the closer wall when `freeAhead < SWERVE_BASE(1400) +
  speed*SWERVE_PER_SPEED(7)`; tie-break gap is `freeAhead/3` (1424-1443).
- Nudge away from other cars about to be hit (not the target it is attacking,
  1445-1469), and bias away from a walled-in side when `freeAhead < LOOK`
  (1471-1479).
- **Separation** (1481-1507): unless actively attacking, spread out from other
  opponents within `SEPARATE_RANGE` (5200); ease the throttle when packed within
  `SEPARATE_CLOSE` (2600).

Speed governor (1520-1560): rather than a fixed frame-count margin, it keeps the
car's own stopping distance in hand — `need = speed² / (2 * cd2CarBrake(cp))`
(`cd2CarBrake` reads the car's combatd2 stats, combatd2.c:300-308). If
`need > room` it brakes. A near-standstill car is exempt so it can get moving
(897-900).

Every branch's steer and thrust are finally **slew-rate limited**
(`STEER_RATE` 105/frame, `THRUST_RATE` 120/frame; full lock is `CD2_STEER_MAX`
352, combatd2.h:52; full throttle `CD2_TMB_THRUST` 4215, combatd2.h:105). Without
this the pivot/brake branches bang-bang the throttle and the car fidgets. Then
`cp->wheel_angle`, `cp->thrust`, `cp->handbrake` and `cp->wheelspin` are written
(1587-1590).

**Weapons** (1592-1625): only when not evading and within `FIRE_RANGE` (9000). It
tries the weapon whose firing tolerance (`fireCone`, per `CD2_WEAPON_DEF` via
`cd2AiCone`, 281-287 — the weapon's own cone, else the global `FIRE_CONE` 420)
the heading error fits. In the "primary window"
(`PRIMARY_MIN` 2200 .. `PRIMARY_RANGE` 14000) it prefers MISSILE, then HOMING,
else MG. All go through `cd2WpnTryFire` (weapons module), so each weapon's own
refire cooldown sets the cadence and the AI can't out-shoot the player. Aim while
attacking is nudged onto the target by `aimErr / AIM_PULL(2)`, but only within
`AIM_PULL_LIMIT` (1100) so the navigation heading keeps priority (1190-1209).

**Observability** (1627-1662): the first slot's values are copied into the
`CD2_AI_DEBUG` snapshot (`cd2AiGetDebug`), and a per-opponent debug line is
logged every 60 frames when `debug_log` is on.

---

## 8. Tuning knobs

All are `#define`s at the top of `opponent.c:54-177` (runtime values — read, not
quoted from prose). Config keys are loaded in `cd2LoadConfig` (combatd2.c:106-110,
114) and clamped 147-153.

Config keys (all in the `[combatd2]` section):

**Opponents are a match setting and default to none.** `ai_opponents` is a count
(0..`CD2_AI_MAX`), so loading the module no longer puts cars on the track by
itself; `cd2MatchOpponents()` returns the effective value for the running match.
It can be set in the ini, stepped from the pause menu (`Opponents: N of 4`), or
overridden for a single headless run with the `CD2_OPPONENTS` environment variable
- which is deliberately **not** written back to the config, so a harness run cannot
rewrite the player's match setting. The old `ai_opponent` 0/1 flag is gone and is
not migrated (it defaulted to on); a stale line in the ini is reported at boot.

| Key | Field | Default | Clamp | Meaning |
|---|---|---|---|---|
| `ai_opponents` | `aiOpponents` | 0 | 0..4 (`CD2_AI_MAX`) | how many opponents this match fields; 0 = none |
| `ai_force_state` | `aiForceState` | 0 | 0..5 | 0 = Auto, else force a `CD2_AI_*` state |
| `ai_debug` | `aiDebug` | 0 | 0/1 | on-screen AI readout |
| `ai_role` | `aiRole` | -1 | -1..3 | -1 = auto round-robin, else force a role |
| `nav_debug` | `navDebug` | 0 | 0/1 | draw the nav graph/routes |
| `ai_damage_taken` | `aiDamageTaken` | 50 | 10..400 | % damage an opponent takes (they were dying too fast) |

The pause menu toggles/cycles all of these (combatd2.c:1706-1806). `ai_damage_taken`
is applied outside opponent.c (core `DamageCar` path), not by the AI.

Engagement / roam ticks (opponent.c):

| Constant | Value | Line | Role |
|---|---|---|---|
| `ENGAGE_RANGE` | 10000 | 55 | range to commit to a fight |
| `ENGAGE_KEEP` | 11000 | 59 | range to stay committed (hysteresis) |
| `DISPERSE_TICKS` | 420 | 63 | opening-spread duration |
| `DISPERSE_LEG` | 26000 | 64 | opening-spread distance |
| `ROAM_MIN` / `ROAM_MAX` | 30000 / 150000 | 65-66 | roam goal node distance band |
| `FLEE_RUN_MIN` / `FLEE_RUN_MAX` | 30000 / 150000 | 68-69 | flee regroup node distance band |
| `GOAL_TICKS` | 1800 | 70 | frames before a roam goal is re-picked |
| `ENGAGE_TICKS` | 4500 | 135 | sustained-aggression frames before break-off |
| `ROAM_INTERRUPT` | 3500 | 140 | a roamer still fights anything this close |
| `ROAM_TICKS` | 300 | 143 | frames spent roaming (the break-off) |
| `ROAM_JITTER` | 90 | 145 | random extra roam frames |
| `STATE_TICKS` | 145 | 148 | frames between re-decisions |
| `MIN_STATE_TICKS` | 150 | 149 | min dwell after a change |
| `STATE_JITTER` | 60 | 128 | random extra decision frames |
| `FLEE_COOLDOWN` | 900 | 96 | frames before it will break contact again |

Movement / avoidance: `SPAWN_OFFSET` 900 (54), `LOOK` 4500 (71),
`AVOID_STEER` 150 (76), `SWERVE_BASE` 1400 (77), `SWERVE_PER_SPEED` 7 (78),
`STEER_DIV` 6 (80), `STEER_DIV_SPEED` 55 (81), `STEER_RATE` 105 (101),
`THRUST_RATE` 120 (106), `PIVOT_DIFF` 1150 (111), `PIVOT_SPEED` 150 (112),
`REVERSE_TICKS` 12 (115), `STUCK_TICKS` 70 (117), `STUCK_SPEED` 5 (118),
`WP_REACH` 700 (119), `LOOKAHEAD_MIN/MAX/PER_SPEED` 1400/6000/12 (120-122),
`SEPARATE_RANGE/CLOSE/STEER` 5200/2600/120 (125-127), `IDLE_TICKS` 200 (150),
`IDLE_SPEED` 15 (151), `FAN_RAYS/STEPS` 5/10 (155-156), `STOP_FRAMES` 8 (161),
`GOVERN_SLACK` 0 (167), `SIDE_MARGIN` 900 (168), `SIDE_BIAS` 384 (170),
`NEAR_BLOCK_MIN` 350 (171), `LOOK_PER_SPEED` 6 (132).

Combat: `EVADE_FRAMES` 85 (85), `FIRE_RANGE` 9000 (87), `FIRE_CONE` 420 (88),
`AIM_PULL_LIMIT` 1100 (90), `AIM_PULL` 2 (91), `MASS_REF` 1200 (92),
`HEALTH_REF` 20000 (93), `DANGER_RANGE` 6500 (94), `STANDOFF` 2600 (95),
`FIRE_DISTANCE` 3200 (97), `STATIONARY` 60 (98), `PRIMARY_MIN/RANGE`
2200/14000 (99-100), `WANDER_LEG` 40000 (130).

Navigation layer: `nav.h:15-21` (`MAX_ROUTE` 64, `WP_STEP` 512, `MAX_NODES` 4096,
`MAX_CARS` 32, `DRAW_RADIUS` 4500); `grid.c:17-23` (`CELL` 512, `MAXDIM` 64,
`MARGIN` 6, `MAXSTEP` 700, `SAMPLE` 160); `flow.c:19-28` (`CELL` 256, `DIM` 96,
`RECENTRE` 2500, `BUDGET` 64, `RESEED_EVERY` 8, `SAMPLE` 120).

---

## 9. Known limitations / gotchas

- **Stale comments.** `ai/ai.h:4-5` and `opponent.c:8-13` name states
  HUNT/WANDER/RECOVER that don't exist in the enum; the real set is §3. Don't
  trust the prose — the enum is the truth.
- **RECOVER is only ever a *reported* state.** It is produced when a car is
  totaled (opponent.c:748), at which point drive returns and does nothing else.
  Forcing `ai_force_state = 5` sets `sState = RECOVER` but there is **no RECOVER
  branch** in the navigator, so it behaves like ROAM. Likewise DISPERSE forced on
  its own just drives out along `wanderHeading`.
- **HARVESTER is inert.** Only FLANKER and AMBUSHER are special-cased
  (973-984); CHASER and HARVESTER take the identical default approach. The enum
  comment "rally points for now" is accurate.
- **Dead `#define`s and fields.** Defined but never used:
  `CD2_AI_PROBE_ANG` (75), `CD2_AI_SWERVE_GAP` (79 — the code now uses
  `freeAhead/3` instead), `CD2_AI_HURT_FLEE` (86), `CD2_AI_FIRE_COOLDOWN` (89 —
  the per-weapon cooldown runs instead), `CD2_AI_NEAR_LOOK` (131),
  `CD2_AI_ENGAGE_JITTER` (129). The `CD2_AI_CAR.fireTimer` and
  `CD2_AI_CAR.engageLimit` fields are set/aliased but never read — aggression
  bursts are *not* jittered per car. Don't tune these expecting an effect.
- **The flow field runs per opponent, not per frame.** `cd2FlowUpdate(64)` is
  inside `cd2AiDrive`, so 4 opponents advance it ~256 cells/frame — more than the
  `CD2_FLOW_BUDGET` name implies.
- **`Random2` is a trap.** It ignores its argument *and* is constant within a
  frame (§4); never use it for jitter. All AI randomness must go through
  `cd2AiRand`/`cd2AiRandSalt`, which require reducing the value yourself (`% n`).
- **CAR_PAD has no enabled/count guard.** `cd2AiOnCarPad` only checks
  `cd2AiIsOpponent` (1798); it will keep blanking the pad of a live opponent even
  after the match is changed to field fewer (or no) opponents, while CAR_STEP
  (which drives it) stops. The car then coasts with no input until the slot is
  reaped (which also stops when the FRAME handler bails at 1704). Lowering the
  opponent count mid-level leaves inert CUTSCENE cars in the world - it takes
  effect on the next level.
- **Route/search capacity.** Routes cap at 64 waypoints (`MAX_ROUTE`); the road A*
  heap is `0x7fffffff`-initialised per search and best-effort on overflow
  (nav.c:576-603, grid.c:161-188). Both use static scratch, so the layer is not
  reentrant — fine because everything runs on the single-threaded CAR_STEP tick.
- **The road graph is rebuilt lazily and re-attempted.** `cd2NavEnsure` retries
  the build whenever `sNodeCount == 0` (nav.c:466-474), because at GAME_START the
  road lumps may not be resident yet. `cd2NavReset` drops it on a level change.
- **Spawn is all-or-nothing per level.** Once opponents exist, `cd2AiOnFrame`
  never spawns more (it only spawns when `live == 0`), so a 3-opponent level
  stays 3 until all die and it re-spawns the full set.
- **Vehicle pool is level-specific.** Because of the NULL-slot fault (§6),
  opponents can only use slots the level loaded all three models for, and never
  the player's own slot. `MAX_CAR_RESIDENT_MODELS` is 8 on this build, 5 on PSX.
