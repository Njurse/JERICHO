# Combat D2

Twisted Metal: Black style arcade handling for REDRIVER2, built as a JERICHO
deep mod. The feel is *engineered, not simulated* — responsive and forgiving
(TMB's north star) — on top of a point-mass rigid body that replaces the
stock wheel/suspension sim:

- **Direct velocity control** — throttle accelerates to `topSpeed`, brake
  decelerates (fast & proportional: strong scrub at speed, smooth taper so it
  never jars to a stop) and reverses up to `reverseSpeed`.
- **Direct yaw control** — steering sets a target yaw rate; turning works at
  zero speed and steering authority never drops mid-slide.
- **Tight Turn** — an acute forced pivot on its own yaw authority, *not*
  steering amplification. It bleeds a little speed so gas + pivot produces a
  short drift-slide; at low speed it spins nearly in place.
- **TMB button layout** (optional, default ON) — with PlayStation face
  buttons as the reference: **Square = Gas, Circle = Brake, X/Cross = Tight
  Turn**, Triangle left unbound for the car (get in/out stays on L3). The
  layout only applies while driving a car, never on foot. If your pad labels
  the *left* face button "X" (Xbox-style), flip **Tight Turn Button** in the
  menu to put the pivot on that button and Gas on X/Cross.
- **Brief, forgiving skids** — high base grip that only sags ~30% at full
  slip, so you never spiral out of control.
- **Short, punchy gears with a tall top** — the rev/gear model is retuned per
  player car so low gears are short and shift quickly, while top gear is tall
  enough that the pitch levels out at the car's top speed instead of revving
  away (tunable via the `CD2_GEAR_*` / `CD2_REV_CEILING` macros in `combatd2.h`).
- **Engine-audio tuners** — the rev + idle channels run louder than stock and
  the pitch slews to redline fast (shifts fall hard). Every knob lives in
  `combatd2.h` as `CD2_REV_RISE_SCALE` / `CD2_REV_DROP_SCALE` (how FAST the
  pitch moves; 4096 = stock lag) and `CD2_SND_*` (mixer pitch/volume: SPU
  pitch 4096 = normal, PSX volume 0 = loudest / −10000 = silent, gain > 4096
  = louder, positive bias = louder) — with a full explanation of units,
  directions and typical ranges in the header itself.
- **Walls absorb momentum** — scenery hits are a hard stop, not a bounce.
- **Weight & Control spread** — derived from each chassis' power-to-weight and
  mass: light cars are instant and agile, trucks are slow but heavy and coast
  longer; collisions stay mass-based so heavies push.

Out of scope for now: turbo meter, energy attacks and ram-damage bonuses.
Button layout: the optional TMB layout remaps the face buttons while driving
(see above), replacing the original car binds while it is active; the
*physical* button → engine-binding stays in the engine's own `config.ini`, so
every bind remains rebindable there — combatd2 never hard-wires a physical
key.

## Weapons (prototype)

The first pass at the car-combat layer, in the module's `weapons.c`:

- **Inventory** — two slots. The **machine gun** is the sidearm: every car
  always has it and it never runs out. The **rocket** is an example primary:
  finite ammo, and the inventory falls back to the MG the moment it empties.
- **Controls** (driving, TMB layout only) — hold **Triangle to fire** (MG
  auto, rocket single-shot), tap **R1 to cycle** MG ↔ armed primary. Binds
  are read from the engine-mapped pad, so the physical buttons stay
  configurable in `config.ini`.
- **Machine gun** — hitscan from the car's nose straight along its heading
  (TM2-style: you steer to aim, no right-stick reticle yet). Each shot draws
  a bright tracer and damages the first car it hits via the engine's
  `ApplyDamage`.
- **Rocket** — a real moving projectile: per-frame flight, drawn body
  (streak + flare) through the engine's `JER_EVENT_DRAW_WORLD` hook, and on
  impact with a car or the ground it explodes (`AddExplosion`) and splashes
  damage onto nearby cars. Damage that tops a car out runs straight into the
  totaled-wreck effects (explosion → flat-black body, wheels off).
- **Getting the rocket** — nothing spawns pickups yet (the `CD2_PICKUP`
  object type + manager are a no-op skeleton in `weapons.c`, ready for the
  map spawner). For now: pause → **Modules → Combat D2 → Debug → Primary**
  toggles the rocket on/off.

Known prototype limits: rockets do not yet collide with buildings (no cheap
module-side wall query), bullet hits don't dent bodies yet, and weapons need
the TMB layout on (Triangle is only free of the car's pedal binds there).

Weapon **impact FX are data-driven per weapon** now (each hit's size, colour,
spin, collision and any bomblet burst). See [`FX.md`](FX.md) for the custom
explosion library and the `JER_EVENT_EXPLOSION_*` engine hooks behind it.

## How it works

All physics runs in one hook, `JER_EVENT_CAR_TORQUE` (the tail of
`StepOneCar`): the module zeros the stock horizontal force and yaw torque,
then writes `linearVelocity[0..2]` and `angularVelocity[1]` directly.
`JER_EVENT_CAR_STEP` snapshots the raw throttle before the stock handbrake
code zeroes it, `JER_EVENT_CAR_PAD` (new engine hook, fired inside
`ProcessCarPad`) lets the module take over the car's pedal semantics for the
TMB layout — the stock face-button assignment is skipped that frame, so the
original binds never double-fire alongside the new ones; `JER_EVENT_CAR_GEARBOX`
retunes the gamesnd rev model (short gears, tall top) and
`JER_EVENT_CAR_ENGINE_SOUND` scales the engine channel pitch/volume — and
`JER_EVENT_GET_WALL_RESTITUTION` (new engine query) makes
walls absorb momentum — the stock collision path is unchanged when combatd2
is off. Vertical motion + roll/pitch stay stock so the car still rides terrain.

The module is one JERICHO mod built from several source files:
`combatd2.c` (core handling + pause menu), `combatd2combat.c` (totaled-car
wreck effects), `combatd2media.c` (gearbox/rev/sound/camera presentation)
and `weapons.c` (the weapon prototype). The weapons run on a small set of
extra hooks: `JER_EVENT_FRAME` (input + projectile sim), `JER_EVENT_DRAW_WORLD`
(projectile/tracer drawing into the real ordering table — an engine hook
added for this) and `JER_EVENT_DRAW_OVERLAY` (the HUD line).

## Tuning

All constants live at the top of `combatd2.h` as `CD2_*` macros (speed =
world-units/frame, yaw = PSX angle units where 4096 = 360°). Live sliders —
Top Speed, Acceleration, Braking, Handling, Grip, Tight Pivot, TMB Buttons
(on/off + tight-button position), Telemetry Log, and three presets — are in
the pause menu under **Modules → Combat D2** (settings persist to
`JERICHO/CONFIG/combatd2.ini`).

## Test launchers (`tools/`)

Windows `.bat` helpers that start the game with a rolled-up setup. They act on
`bin\Release_dev\` directly and the module must be enabled in
`bin\Release_dev\JERICHO\CONFIG\modlist.ini`.

These files are the source of truth; copies also sit next to the executable in
`bin\Release_dev\` for double-clicking. `bin/` is gitignored, so re-copy after
changing one here:

```
cp JERICHO/MODS/combatd2/tools/launch_*.bat src_rebuild/bin/Release_dev/
```

- `launch_tar_random.bat` — Take-A-Ride with a random city, car, weather and time.
- `launch_mp_chicago_semi.bat` — Chicago's multiplayer arena, as the semi when its
  data is present (falls back to the school bus).
- `launch_mp_random_mix.bat` — multiplayer arena with a **random cross-city
  import** and a mixed roster. The player gets a random local slot or, half the
  time, a random foreign car (`player_model`); 1-2 vehicles are imported from a
  randomly chosen *other* city into random resident slots. The AI opponents pick
  their car at spawn by enumerating the resident slots the level actually loaded
  and taking a salted random one, so they mix the imported vehicles in by
  themselves — as does ambient traffic, which draws from slots 0..4. Run it with
  the argument `dry` to see the roll without launching or touching the config.

`launch_mp_random_mix.bat` **overwrites** `JERICHO/CONFIG/carhacks.ini` when it
runs (the cross-city hack is off by default; the launcher switches it on for the
session).

**Never use `-car slot9`.** It is model 11, the slot the game reserves for a
content-override truck, and no city ships data for it — the game dies during load
(after `LUMP_CAR_MODELS`, no dump). Both random launchers skip it deliberately.

## Validation checklist

- TMB layout: while driving, Square gasses, Circle brakes, X/Cross pivots;
  on foot the ped controls are unchanged. Flip Tight Turn Button if the pad
  labels the left face button "X".
- Tight Turn: hard pivot, spins nearly in place at low speed, gas + pivot
  slides instead of stopping dead.
- High-speed stop: brakes scrub quickly but taper — no jarring dead stop.
- Wall hit at speed: hard stop, minimal bounce, no wall-spin; still controllable.
- Vehicle spread: a light car out-accelerates and out-turns a heavy one.
- MG: hold Triangle while driving — tracers stream from the nose, cars ahead
  smoke and (over time) total out; the wreck still explodes/flat-blacks.
- Rocket: pause → Modules → Combat D2 → Debug → **Primary** (grants 10),
  hold Triangle to fire one; it flies visibly, explodes on a car/ground and
  damages everything nearby. Tap R1 to swap back to the MG; empty rocket ammo
  auto-returns to the MG. **Primary** again clears the slot.
