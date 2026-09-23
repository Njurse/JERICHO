# Caine's Crossfire

A **total conversion** of REDRIVER2 into a Twisted Metal-style car-combat game,
built as a JERICHO deep mod. The goal is the whole experience, not just
handling: a roster of vehicles with their own stats and special weapons, a
pre-match select flow, and Twisted Metal: Black style arcade handling. The feel
is *engineered, not simulated* — responsive and forgiving (TMB's north star) —
on top of a point-mass rigid body that replaces the stock wheel/suspension sim
(the suspension itself is kept, for visual roll/pitch only).

- **The roster** — vehicles are **profiles** (identity + `(city, model)`
  mapping, 1..5 stats, a special weapon, and native-physics overrides). See
  [`PROFILES.md`](PROFILES.md); each profile's special is in
  [`SPECIALS.md`](SPECIALS.md). A profile is one file under `profiles/rows/`.

- **Direct velocity control** — throttle accelerates to `topSpeed`, brake
  decelerates (fast and proportional, then tapers) and reverses up to
  `reverseSpeed`.
- **Direct yaw control** — steering sets a target yaw rate; turning works at
  zero speed and steering authority never drops mid-slide.
- **Tight Turn** — an acute forced pivot on its own yaw authority, not
  steering amplification. It bleeds a little speed so gas + pivot is a short
  drift-slide; at low speed it spins nearly in place.
- **Optional TMB face-button layout** (default ON) — with PlayStation face
  buttons as the reference: **Square = Gas, Circle = Brake, X/Cross = Tight
  Turn**, Triangle left unbound for the car (get in/out stays on L3). It only
  applies while driving a car, never on foot. If your pad labels the *left*
  face button "X" (Xbox-style), flip **Tight Turn Button** in the menu. The
  layout is only a *read* override — the physical button → engine binding stays
  in the engine's own `config.ini`, so every bind remains rebindable.
- **Brief, forgiving skids** — high base grip that only sags ~30% at full slip.
- **Walls absorb momentum** — scenery hits kill only the *into-wall* velocity
  and let the car scrape along, instead of stopping dead or bouncing.
- **Weight & control spread** — derived per chassis from power-to-weight and
  mass: light cars are instant and agile, trucks are heavy but coast longer;
  collisions stay mass-based so heavies push.
- **Presentation tuners** — the rev/gearbox model and engine channel are
  retuned per car; the knobs (`CD2_GEAR_*`, `CD2_REV_*`, `CD2_SND_*`) live in
  `cainescrossfire.h` / `cainescrossfiregearbox.c` / `cainescrossfireenginesnd.c`. Time-scale and SPU pitch both use
  **4096 = stock/normal**; PSX volume **0 = loudest**, **−10000 = silent**.
  See [`SOUNDS.md`](SOUNDS.md) for the sample/bank API.
- **Car combat** — the direct-velocity handling plus a data-driven weapon layer
  (a machine-gun sidearm and finite primaries), six **vehicle specials**, and
  totaled-car wreck effects. The CC select flow (`-ccmenu`) picks an arena and a
  vehicle before the match. The damage model is tuned so a car lasts: one global
  weapon-damage percentage, a scenery-impact threshold (scrapes cost nothing) and
  a car-vs-car aggressor rule (the car doing the ramming takes no damage). The
  locked target's health shows as a small bar under its name. See
  [`HANDLING.md`](HANDLING.md) § *The damage model*.

Still to come: tournament brackets, a garage, and the rest of the total
conversion.

## Enabling

Enable the module in `JERICHO/CONFIG/modlist.ini` (the file the **Options →
JERICHO** frontend also rewrites):

```
Caine's Crossfire = 1
```

`JERICHO/MODS/cainescrossfire/mod.toml` ships `default-enabled = true`, so a build
that does not list the module still runs it; an explicit `cainescrossfire = 0` turns
it off. With the module off, the stock physics and collision path run
unchanged. Live settings persist to `JERICHO/CONFIG/cainescrossfire.ini` (loaded and
saved by `cainescrossfire.c` `cd2LoadConfig` / `cd2SaveConfig`).

## Documentation

- [`HANDLING.md`](HANDLING.md) — the point-mass handling model: what Caine's Crossfire
  owns vs. the engine, the TM2 / TM Black post-mortem mapping, the wall-scrape
  fix, and where every `CD2_*` handling/physics macro lives.
- [`FX.md`](FX.md) — the parametric explosion library and the
  `JER_EVENT_EXPLOSION_*` hooks: per-weapon FX profiles, the barrage sequencer,
  and the volley / freeze / scatter weapon behaviours.
- [`SOUNDS.md`](SOUNDS.md) — Driver 2's three audio layers, the sample-play
  API, the sound banks, and candidate weapon sample indices.
- [`AI.md`](AI.md) — the prototype opponent AI (`ai/`): states, roles,
  navigation and the on-screen readout.
- [`MOUNTED_CREW.md`](MOUNTED_CREW.md) — the driver/gunner who lean out of the
  window to fire crew weapons: the `leanOut` bitmask, the selected/fired request
  and its OR rule, the get-out/hold/get-in ped lifecycle, firing from the
  window, the wreck bail-out (run away on fire) and the player-death camera
  hold.
- [`CREW_POSES.md`](CREW_POSES.md) — the crew's bone contract and how to author
  a pose outside the code: the 23-bone order/hierarchy, the 4096-per-turn
  parent-relative ZYX angles, the two `jer_anim` channels and their traps, the
  measured model-facing offset, and the paste-ready baseline dump.
- [`MOTION.md`](MOTION.md) - the procedural motion layers: the engine-idle shudder,
  the pitch-back under power, the squat and the nose-bob, the vehicle classes, and the
  one clamp that stops two subsystems adding up badly.
- [`KNOCK.md`](KNOCK.md) - the impact layer: bucking and rocking a car for effect.
- [`TURBO.md`](TURBO.md) - the turbo meter, the trigger, and where the numbers came
  from.
- [`FACTIONS.md`](FACTIONS.md) — the five teams (`factions/`): the rows and their
  colours, the roster and who drives what, the stance table, how a car is
  assigned its team, and which attributes are deliberately not read yet.
- [`carhacks/CROSS_CITY.md`](carhacks/CROSS_CITY.md) — what a cross-city
  vehicle import has to pull across, and why colours need more than geometry.
- [`carhacks/FORMATS.md`](carhacks/FORMATS.md) — the reverse-engineered
  `.LEV`/`.LCF` layouts (citylumps, the 4-byte aligned segment walk,
  `LUMP_CAR_MODELS`/`LUMP_PALLET`, the car draw path) plus Python recipes to
  re-measure them.

## How it works

All handling runs in one engine hook, `JER_EVENT_CAR_TORQUE` (the tail of
`StepOneCar`): the module zeroes the stock horizontal force and yaw torque,
then writes `linearVelocity[0..2]` and `angularVelocity[1]` directly. It also
attaches to:

- `JER_EVENT_CAR_PAD` — fired inside `ProcessCarPad`; the module takes over the
  car's pedal semantics for the TMB layout and the stock face-button assignment
  is skipped that frame, so the original binds never double-fire.
- `JER_EVENT_CAR_STEP` (snapshots the raw throttle before the stock handbrake
  code zeroes it; also runs the freeze lock) plus `JER_EVENT_CAR_GEARBOX` /
  `JER_EVENT_CAR_ENGINE_SOUND` (retune the gamesnd rev model and the engine
  channel).
- `JER_EVENT_GET_WALL_RESTITUTION` — makes walls absorb momentum.
- `JER_EVENT_GET_PHYSICS_PARAMS` — re-tunes gravity, angular damping and the
  suspension springs per car.
- The weapon layer's `JER_EVENT_FRAME` (input + projectile sim),
  `JER_EVENT_DRAW_WORLD` (projectile/tracer drawing into the ordering table)
  and `JER_EVENT_DRAW_OVERLAY` (the HUD line).

Vertical motion and roll/pitch stay stock, so the car still rides terrain.
Remove the `JERICHO/MODS/cainescrossfire` folder and the game is stock again — the
engine only calls the mod through the JERICHO registry. The full hook-to-field
mapping is in [`HANDLING.md`](HANDLING.md) and [`FX.md`](FX.md).

## Weapons

Two inventory slots: the **machine gun** is the always-carried sidearm
(infinite); primaries (**MISSILE, SEEKER, CLUSTER, ZOOMY, FREEZE, SHOTGUN** and
the hidden MINE) are finite and fall back to the MG the moment they empty.
Controls, read from the engine-mapped pad (`weapons/core/weapons.c:94-97`):

- **L2** (left trigger) — hold to fire the machine gun, auto fire.
- **R2** (right trigger) — fire the selected primary, one shot per press; with
  no primary armed it falls back to the MG so the trigger is never dead.
- **R1 / L1** — cycle to the next / previous carried weapon (edge-triggered).

Weapons are data-driven: each is one `CD2_WEAPON_DEF` row, grouped by a
functional class under `weapons/`. The shared field set and the registry API
are in [`weapons/core/weapon.h`](weapons/core/weapon.h); the impact FX profiles
and the volley / freeze / scatter behaviours are in [`FX.md`](FX.md). Nothing
spawns drive-over pickups yet (the map spawner is not wired) — for testing,
grant weapons from the pause menu (**Modules → Caine's Crossfire → Weapons...**).

Two additions exist mainly to put on a show:

| Weapon | What it is |
| --- | --- |
| **SMG** | A burst sidearm: one trigger = **6 shots**, 2 frames apart, each homing only very weakly, then a ~**1.1s** pause before the next volley. Fast, close-range spray (`weapons/projectile/smg.c`). |

The SMG is an ordinary primary — in the cycle, grantable, `all_weapons` covers
it — and it is fireable through the scripted debug driver as `fire:smg`.

## Layout

| Path | Holds |
|---|---|
| `cainescrossfire.c` | core: config load/save, per-car state, per-vehicle stats, the car-identity helpers, the bootstrap + pursuit hooks, and the module entry that wires everything |
| `cainescrossfire_internal.h` | declarations shared between this module's source files (and a file map) |
| `cainescrossfiresim.c` | the point-mass handling model (TMB pad override, CAR_STEP/TORQUE, roll limit/recovery, wall restitution, physics params, the render lean) |
| `cainescrossfirerespawn.c` | wreck age-out: respawn timers, traffic tumble, wreck mass, the death bang |
| `cainescrossfiredamage.c` | damage scaling (scenery + car-vs-car) |
| `cainescrossfiremenu.c` | the pause menu |
| `cainescrossfirewreckfx.c` | the wreck edge: `cd2CarTotaled`, the explosion, the kill-credit toast, the wreck toss |
| `cainescrossfirecarfx.c` | totaled-car presentation (flat black, wheels gone, engine muted, body dropped) |
| `cainescrossfiregearbox.c` | the engine gearbox / rev-curve tuner |
| `cainescrossfireenginesnd.c` | the engine rev + idle channel tuner |
| `cainescrossfirecamerafx.c` | chase framing + speed FOV pull |
| `cainescrossfire.h` | every compile-time tunable (`CD2_*` macros) and the shared structs |
| `cd2debug.c` | temporary scripted debug driver (`tools/cc_debug.example.txt` is the file format) |
| `weapons/` | the weapon framework: `core/` (registry + inventory), `raycast/` (machine gun), `projectile/`, `shotgun/`, `drops/`, `aoe/`, `fx/` |
| `ai/` | the prototype opponent AI (`opponent.c` brain; `nav.c` / `grid.c` / `flow.c` navigation) |
| `factions/` | the five teams: the registry, the roster, the stance table and the per-car assignment (`factions.c`; see [`FACTIONS.md`](FACTIONS.md)) |
| `carhacks/` | vehicle-availability hacks, plus the two format docs |
| `tools/` | the test launchers and the arena smoke test |
| `mod.toml` | package metadata (`id`, `default-enabled`) |

## Test tooling (`tools/`)

Windows helpers that boot the game with a rolled-up setup. They act on
`bin\Release_dev\` and need the Caine's Crossfire module enabled in
`bin\Release_dev\JERICHO\CONFIG\modlist.ini`. These files are the source of
truth; copies also sit next to the executable for double-clicking. `bin/` is
gitignored, so re-copy after editing one:

```
cp JERICHO/MODS/cainescrossfire/tools/launch_*.bat src_rebuild/bin/Release_dev/
```

### `arena_test.sh` — multiplayer smoke test

A headless(ish) smoke test that boots a **multiplayer map** (the two-per-city
arenas are small and closed, which makes them far better for combat testing
than the open city missions) with a random **city / car / weather / time**, so
each run exercises a different combination:

```
./arena_test.sh [seconds] [extra args...]
./arena_test.sh                # 45s, random everything
./arena_test.sh 90             # 90s run
./arena_test.sh 45 -car slot5  # ...but pin the car
```

The car is always the validated `-car slot1..slot10` form; the `CAR` env var
overrides the random pick, and extra args are appended to the launch line. It
launches with `-nointro -mp <arena> -level <city> -car <slot> -weather <w>
-time <t>`, sleeps, then:

- **kills by PID only** (`taskkill //F //PID <pid>`) — it prints that exact
  command — never by image name (see *Safety rules*);
- treats a process that exited on its own before the timer as a **CRASH** (the
  old script never noticed a dead game and reported "0 errors");
- notes any pre-existing `REDRIVER2.dmp` and flags a **new** one as a crash;
- **snapshots** `REDRIVER2.log` to `arena_test_<city>_<weather>_<time>_<stamp>.log`
  (**it does not delete the log**) and greps the snapshot: `verdict: reached
  GAMEPLAY` when a gameplay marker is found, otherwise the last log line, plus
  counts of active modules and crash markers.

### Launchers

All five `cd` into `bin\Release_dev\`, then `start` `REDRIVER2_dev.exe`:

| Launcher | What it does |
|---|---|
| `launch_tar_random.bat` | Take-a-Ride with a random city, a random slot (1..8 and 10), a random weather and time: `-level <city> -car slot<N> -weather <w> -time <t> -gamemode takeadrive`. |
| `launch_mp_chicago_semi.bat` | Chicago's **multiplayer arena 1** (`-mp 1`; `-mp 0` is the other arena), player spawned as the **semi** when its data is present. |
| `launch_tar_chicago_semi.bat` | **Single-player** Chicago, player spawned as the semi when its data is present. |
| `launch_mp_random_mix.bat` | Multiplayer arena with a random cross-city import and a mixed roster; **overwrites** `carhacks.ini`; supports a `dry` argument. |
| `launch_mp_foreign_car.bat` | The same arena mix with the coin flip removed: the player is **always** in a foreign car, drawn from the source city's **whole usable roster** — civilian bodies 0..4 as well as the special ones 8, 9, 10 and 12, plus 11 when the source city has it (Chicago does not) — selected with `-car <model>`. Civilian bodies are imported slot-for-slot over slots 0..4 and special bodies into spare slot 5, because carhacks only writes a model number and the engine then spawns the player in whichever resident slot already holds it. Supports `dry`. |
| `launch_cc_select.bat` | Boot straight into the **CC select flow** (`-ccmenu`): pick an arena (the four cities), then a vehicle (every registered profile), then the match starts. No `-level`, so the game comes up in the frontend. Supports `dry`. |

`launch_mp_foreign_car.bat` is the reliable way to reproduce the outstanding
cross-city rendering limitation: a foreign vehicle carries the other city's
geometry and palettes, but its polygons name **that** city's texture pages, which
this level has not loaded — so it does not yet render correctly. That work is
described in `carhacks/CROSS_CITY.md`.

**They share one config file.** Both `launch_mp_*` launchers write the same
`JERICHO/CONFIG/carhacks.ini`, so two runs at once will clobber each other's roll
(and a game already in progress picks the change up on its next level load). Run
one at a time, or check the file after launching if the cars look wrong.


Both `*_chicago_semi.bat` launchers want the semi at
`DRIVER2\LEVELS\CHICAGO\CARMODEL_11_clean.dmodel`. This install ships only the
school bus (`CARMODEL_10_clean.dmodel`), and forcing a model with no data
crashes the game during load, so each script checks the file first and, if it is
missing, spawns the school bus (`-car slot8`) instead — drop the semi's file in
and the same script spawns the semi.

`launch_mp_random_mix.bat` rolls a random arena city and a **different** city to
import from, then imports 1-2 foreign vehicles into random resident slots (0..6;
0..4 feed ambient traffic, 5..6 are spare capacity) and — half the time — gives
the player a random foreign car instead of a local slot. The AI opponents pick
their car at spawn by enumerating the resident slots the level actually loaded,
and ambient traffic draws from slots 0..4, so both mix the imports in on their
own. Run it with the argument `dry` to print the roll, the `carhacks.ini` it
*would* write and the launch line, without writing or launching anything. When
it *does* run it **overwrites** `bin\Release_dev\JERICHO\CONFIG\carhacks.ini`
(the cross-city hack is off by default; the launcher switches it on for the
session).

### Standing hazard: never use `-car slot9`

`-car slot9` is car **model 11**, the slot the game reserves for a
content-override truck (`FEmain.c`, "remove truck"). **No city ships data for
model 11 in Chicago**, so forcing it kills the game during load (right after
`LUMP_CAR_MODELS`, no dump). It is a real vehicle in Havana, Rio and Vegas.
Every random launcher above skips slot 9 deliberately — do not "fix" them into
picking it. The slot form is the only safe one: it is mapped through
`carNumLookup[level]` and bounds-checked, while a raw `-car <n>` is **not**, so
any index past a level's loaded car pool crashes the same way.

### Safety rules (do not break when testing)

- **Kill by PID only.** The game never self-exits, and killing by image name
  would also take down a session the user is running by hand. `arena_test.sh`
  prints the exact `taskkill //F //PID <pid>` it used.
- **Never delete `REDRIVER2.log`.** The user's own sessions write that file too.
  Snapshot it (copy to a per-run name, as `arena_test.sh` does) instead.
- `REDRIVER2.log` is **truncated at session start and flushed at close** (see
  `carhacks/FORMATS.md` §8), so a log snapshotted right after a `taskkill` can
  be cut off mid-session — treat a missing tail accordingly.

## Validation checklist

- **TMB layout**: while driving, Square gasses, Circle brakes, Cross pivots; on
  foot the ped controls are unchanged. Flip **Tight Turn Button** if the pad
  labels the left face button "X".
- **Tight Turn**: hard pivot, spins nearly in place at low speed, gas + pivot
  slides instead of stopping dead.
- **High-speed stop**: brakes scrub quickly but taper — no jarring dead stop.
- **Wall hit at speed**: hard stop, minimal bounce, no wall-spin; still
  controllable.
- **Vehicle spread**: a light car out-accelerates and out-turns a heavy one.
- **Machine gun**: hold **L2** while driving — tracers stream from the nose,
  cars ahead smoke and (over time) total out; the wreck still explodes and
  flat-blacks.
- **Primary**: pause → **Modules → Caine's Crossfire → Weapons...**, toggle **MISSILE**
  to grant it (toggling again clears the slot), tap **R2** to fire one — it
  flies visibly, explodes on a car or the ground and damages everything nearby.
  With no primary armed, R2 falls back to the MG; an empty primary auto-returns
  to the MG. **R1 / L1** cycle carried weapons.
